/* get_dsa_info.c - Get DSA info given its distinguished name */

#ifndef lint
static char *rcsid = "$Header: /f/osi/quipu/RCS/get_dsa_info.c,v 7.5 91/03/09 11:56:59 mrose Exp $";
#endif

/*
 * $Header: /f/osi/quipu/RCS/get_dsa_info.c,v 7.5 91/03/09 11:56:59 mrose Exp $
 *
 *
 * $Log:	get_dsa_info.c,v $
 * Revision 7.5  91/03/09  11:56:59  mrose
 * update
 * 
 * Revision 7.4  91/02/22  09:39:25  mrose
 * Interim 6.8
 * 
 * Revision 7.3  90/10/17  11:54:22  mrose
 * sync
 * 
 * Revision 7.2  89/12/19  16:20:36  mrose
 * sync
 * 
 * Revision 7.1  89/11/24  16:21:59  mrose
 * sync
 * 
 * Revision 7.0  89/11/23  22:17:42  mrose
 * Release 6.0
 * 
 */

/*
 *                                NOTICE
 *
 *    Acquisition, use, and distribution of this module and related
 *    materials are subject to the restrictions of a license agreement.
 *    Consult the Preface in the User's Manual for the full terms of
 *    this agreement.
 *
 */


#include "quipu/util.h"
#include "quipu/read.h"
#include "quipu/entry.h"
#include "quipu/dua.h"
#include "quipu/bind.h"
#include "quipu/connection.h"

extern LLog * log_dsap;
extern DN mydsadn;
extern int dn_print();

struct oper_act	* oper_alloc();
struct di_block	* di_alloc();
struct oper_act	* make_get_dsa_info_op();
extern Attr_Sequence entry_find_type();
extern char * index();

/*
*  This routine is used to read the info (including presentation address)
*  for a dsa given its distinguished name.
*  This is called during the DSA initialisation, to find the name THIS dsa.
*/

int	  get_dsa_info(dn, dn_stack, err, di_p)
DN		  dn;
struct dn_seq	* dn_stack;
struct DSError	* err;
struct di_block	**di_p;
{
    struct di_block	* di_tmp;
    struct di_block	* di_lookup;
    struct oper_act	* on = NULLOPER;
    int 		  res;

    DLOG (log_dsap,LLOG_TRACE,("get_dsa_info()"));

    if (dn_in_dnseq(dn, dn_stack))
    {
        LLOG (log_dsap,LLOG_NOTICE,("get_dsa_info - loop detected"));
	err->dse_type = DSE_SERVICEERROR;
	err->dse_un.dse_un_service.DSE_sv_problem = DSE_SV_LOOPDETECT;
	return(DS_X500_ERROR);
    }

    /* if asking about me, use my cached entry */
    if (dn_cmp (dn,mydsadn) == 0)
    {
        LLOG (log_dsap,LLOG_NOTICE,("get_dsa_info - referring to self :-)"));
	err->dse_type = DSE_SERVICEERROR;
	err->dse_un.dse_un_service.DSE_sv_problem = DSE_SV_DITERROR;
	return(DS_X500_ERROR);
    }


    (*di_p) = di_alloc();
    (*di_p)->di_type = DI_TASK;
    (*di_p)->di_dn = dn_cpy(dn);
    (*di_p)->di_target = NULLDN;
    (*di_p)->di_reftype = RT_SUBORDINATE;
    (*di_p)->di_rdn_resolved = CR_RDNRESOLVED_NOTDEFINED;
    (*di_p)->di_aliasedRDNs = CR_NOALIASEDRDNS;

    /*
    *  Check for a GetDSAInfo operation already in the pipeline.
    */
    for(di_tmp=deferred_dis; di_tmp != NULL_DI_BLOCK; di_tmp=di_tmp->di_next)
    {
	if(dn_cmp(dn, di_tmp->di_dn) == 0)
	{
	    (*di_p)->di_state = DI_DEFERRED;
	    (*di_p)->di_entry = NULLENTRY;

	    /* link to the performing operation */
	    (*di_p)->di_perform = di_tmp->di_perform;

	    /* Add to wake list leaving global block first to be woken */
	    (*di_p)->di_wake_next = di_tmp->di_wake_next;
	    di_tmp->di_wake_next = (*di_p);

	    DLOG(log_dsap, LLOG_DEBUG, ("Found global deferred di_block:"));
#ifdef DEBUG
	    di_list_log((*di_p));
#endif
	    return(DS_CONTINUE);
	}
    }

    if ((res = really_find_entry(dn, TRUE, dn_stack, FALSE, &((*di_p)->di_entry), err, &(di_lookup))) == DS_OK) 
	/* is it really OK ??? */
	if (((*di_p)->di_entry ->e_data == E_TYPE_CONSTRUCTOR) 
		|| ((*di_p)->di_entry->e_dsainfo == NULL)
		|| ((*di_p)->di_entry->e_dsainfo->dsa_addr == NULLPA)) {
		DN dn_found;
		DLOG(log_dsap, LLOG_NOTICE, ("rfe returned a constructor"));
		dn_found = get_copy_dn((*di_p)->di_entry);
		res = constructor_dsa_info(dn_found,dn_stack,FALSE,(*di_p)->di_entry,err,&(di_lookup));
		dn_free (dn_found);
	} else
		(*di_p)->di_entry->e_refcount++;

    switch (res)
    {
    case DS_OK:
	/* really_find_entry has found the entry and placed it in di_entry */
	DLOG(log_dsap, LLOG_DEBUG, ("get_dsa_info - really_fe returns DS_OK"));
	(*di_p)->di_state = DI_COMPLETE;
#ifdef DEBUG
	di_list_log((*di_p));
#endif
	return(DS_OK);

    case DS_CONTINUE:
	/*
	*  A list of di_blocks (di_lookup) has been generated by get_dsa_info.
	*  These should be used to chain the get_dsa_info operation.
	*  Attempt to generate an operation using the di_blocks returned
	*  and if successful, defer the current di_block to it.
	*/
	DLOG(log_dsap, LLOG_DEBUG, ("gdi rfe returned DS_CONT:"));
#ifdef DEBUG
	di_list_log(di_lookup);
#endif
	if((on = make_get_dsa_info_op(dn, di_lookup)) == NULLOPER)
	{
	    /* Flake out screaming */
	    LLOG(log_dsap, LLOG_EXCEPTIONS, ("make_get_dsa_info_op failed for get_dsa_info"));
	    free((char *)*di_p);
	    (*di_p) = NULL_DI_BLOCK;
	    err->dse_type = DSE_SERVICEERROR;
	    err->ERR_SERVICE.DSE_sv_problem = DSE_SV_UNABLETOPROCEED;
	    return(DS_X500_ERROR);
	}

	if(oper_chain(on) != OK)
	{
	    /* Flake out screaming */
	    LLOG(log_dsap, LLOG_NOTICE, ("send_op failed for get_dsa_info"));
	    free((char *)*di_p);
	    (*di_p) = NULL_DI_BLOCK;
	    err->dse_type = DSE_SERVICEERROR;
	    err->ERR_SERVICE.DSE_sv_problem = DSE_SV_UNABLETOPROCEED;
	    return(DS_X500_ERROR);
	}

	di_tmp = di_alloc();
	di_tmp->di_dn = dn_cpy(dn);
	DLOG(log_dsap, LLOG_DEBUG, ("get_dsa_info allocates di_block with dn[%x]", di_tmp->di_dn));
	di_tmp->di_state = DI_DEFERRED;
	di_tmp->di_type = DI_GLOBAL;
	di_tmp->di_perform = on;
	on->on_wake_list = di_tmp;	/* wake globals first */

	(*di_p)->di_state = DI_DEFERRED;
	(*di_p)->di_perform = on;

	/* Add to wake list leaving global block first to be woken */
	(*di_p)->di_wake_next = NULL_DI_BLOCK;
	di_tmp->di_wake_next = (*di_p);

	di_tmp->di_next = deferred_dis;
	deferred_dis = di_tmp;

	DLOG(log_dsap, LLOG_DEBUG, ("gdi DS_CONT: generated:"));
#ifdef DEBUG
	di_list_log((*di_p));
#endif
	return(DS_CONTINUE);

    case DS_X500_ERROR:
	/* something wrong with the request - err should be filled out */
	DLOG(log_dsap, LLOG_DEBUG, ("gdi X500_ERROR"));
	free((char *)*di_p);
	(*di_p) = NULL_DI_BLOCK;
	return(DS_X500_ERROR);

    default:
	LLOG(log_dsap, LLOG_EXCEPTIONS, ("Unexpected return from read_dsa_info"));
	free((char *)*di_p);
	(*di_p) = NULL_DI_BLOCK;
	err->dse_type = DSE_SERVICEERROR;
	err->ERR_SERVICE.DSE_sv_problem = DSE_SV_UNABLETOPROCEED;
	return(DS_X500_ERROR);
    }
}


dsa_info_result_wakeup(on)
struct oper_act	* on;
{
EntryInfo	* ent_res;
Entry		  di_ent;
struct di_block	* di;
struct di_block	* next_di;
struct di_block	**di_p;
Entry		  cache_dsp_entry();

    DLOG(log_dsap, LLOG_DEBUG, ("dsa_info_result_wakeup()"));

    /*
    *  Cache the entry returned, flake out if it is not unravellable,
    *  otherwise grab a reference to the unravelled entry.
    */

    ent_res = &(on->on_resp.di_result.dr_res.dcr_dsres.res_rd.rdr_entry);
    if((di_ent = cache_dsp_entry (ent_res)) == NULLENTRY)
    {
	LLOG (log_dsap, LLOG_EXCEPTIONS, ("dsa_info_result_wakeup - cache_dsp_entry failure"));
	/* This could mean the cached entry was a SLAVE - if so why were we 
	 * doing a get dsa info ?
         */
	dsa_info_error_wakeup(on);
	return;
    }

    DLOG(log_dsap, LLOG_DEBUG, ("dsa_info_result_wakeup - cached dsa_info"));

    /*
    *  First block on the wake up list should be the global marker.
    *  Verify this and remove it.
    */
    if(on->on_wake_list->di_type != DI_GLOBAL)
    {
	LLOG(log_dsap, LLOG_EXCEPTIONS, ("First di_block to wake not global"));
    }
    else
    {
	di_p = &(deferred_dis);
	for(di = deferred_dis; di != NULL_DI_BLOCK; di=(*di_p))
	{
	    if(di == on->on_wake_list)
		break;

	    di_p = &(di->di_next);
	}
	if(di == NULL_DI_BLOCK)
	{
	    LLOG(log_dsap, LLOG_EXCEPTIONS, ("Global di_block wasn't on global list"));
	}
	else
	{
	    (*di_p)=di->di_next;
	}
    }

    DLOG(log_dsap, LLOG_DEBUG, ("dsa_info_result_wakeup - dealt with global block"));

    for(di = on->on_wake_list->di_wake_next; di != NULL_DI_BLOCK; di = next_di)
    {
	next_di = di->di_wake_next;
	di->di_state = DI_COMPLETE;
	di->di_entry = di_ent;
	di_ent->e_refcount++;

	switch(di->di_type)
	{
	case DI_OPERATION:
	    if (di->di_oper == NULLOPER) {
		di_free (di);
		break;
	    }
	    if(di->di_oper->on_state == ON_DEFERRED)
	    {
	        if (oper_chain(di->di_oper) != OK) {
			LLOG (log_dsap,LLOG_EXCEPTIONS, ("oper_chain failed in dsa_info_wakeup"));
			di_free(di);
		}
	    }
	    break;

	case DI_TASK:
	    task_dsa_info_wakeup(di);
	    di_free(di);
	    break;

	default:
	    LLOG(log_dsap, LLOG_EXCEPTIONS, ("get_dsa_info_aux - unknown di-type %d",di->di_type));
	    oper_extract(on);
	    return;
	}
    }

    DLOG(log_dsap, LLOG_DEBUG, ("dsa_info_result_wakeup - woke all blocks"));

    /*
    *  Everthing should have been woken up by now so the di_blocks on
    *  the wake list and the operation itself can be extracted.
    */
    di_free(on->on_wake_list);

    oper_extract(on);
}

dsa_info_error_wakeup(on)
struct oper_act	* on;
{
struct DSError	* err = &(on->on_resp.di_error.de_err);
struct di_block	* di;

    /*
    *  Error can fall into 3 categories:
    *    1) Problem with remote DSA performing operation - try another;
    *    2) A referral error - follow the referral;
    *    3) An error with the operation itself;
    */

    switch(err->dse_type)
    {
    case DSE_NOERROR:
	LLOG(log_dsap, LLOG_NOTICE, ("dsa_info_error_wakeup - No Error!"));
	dsa_info_fail_wakeup(on);
	return;
    case DSE_REFERRAL:
	LLOG(log_dsap, LLOG_NOTICE, ("dsa_info_error_wakeup - DAP Referral!"));
    case DSE_DSAREFERRAL:
	if(oper_rechain(on) == OK)
	    return;
	/* Fall through */
    default:
	DLOG(log_dsap, LLOG_DEBUG, ("dsa_info_error_wakeup - Assuming all non-referral errors are to be propogated"));
	/* Lose the di_block */
	for(di=on->on_wake_list; di!=NULL_DI_BLOCK; di=di->di_wake_next)
	{
	    switch(di->di_type)
	    {
	    case DI_OPERATION:
		LLOG(log_dsap, LLOG_EXCEPTIONS, ("Should wake oper"));
		oper_log(di->di_oper);
		break;
	    case DI_TASK:
		LLOG(log_dsap, LLOG_EXCEPTIONS, ("Should wake task"));
		task_log(di->di_task);
		break;
	    case DI_GLOBAL:
		LLOG(log_dsap, LLOG_EXCEPTIONS, ("Should wake global"));
		break;
	    default:
		LLOG(log_dsap, LLOG_EXCEPTIONS, ("dsa_info_error_wakeup - invalid di_type"));
		break;
	    }
	}
	return;
    }
}


dsa_info_fail_wakeup(on)
struct oper_act	* on;
{
    /*
    *  Last attempt to get dsa info failed somehow.
    *  If there are any more "di_block"s to attempt it must be
    *  worth a go (perhaps this depends on the failure which
    *  has occurrred).
    */
    if (on -> on_resp.di_type == DI_ERROR) {
	    pslog (log_dsap,LLOG_EXCEPTIONS,"Remote dsainfo error",dn_print,
		   (caddr_t) on -> on_req.dca_dsarg.arg_rd.rda_object);
	    log_ds_error (& on -> on_resp.di_error.de_err);
    }

    if(on->on_dsas)
    {
	if(oper_chain(on) == OK)
	    return;
    }

    if(on->on_dsas)
    {
	/* oper_chain must be awaiting deferred di_blocks */
	return;
    }

    /*
    *  There is nowhere left to chain this operation to so no way to get
    *  the dsa info required. Walk through the wake up list extracting,
    *  waking things up and tidying up afterwords.
    */
}

char * get_entry_passwd (as)
Attr_Sequence as;
{
extern AttributeType at_password;
Attr_Sequence at;

	if ((at = as_find_type (as,at_password)) == NULLATTR) 
		return (NULLCP);

	if (at->attr_value == NULLAV)
		return (NULLCP);

	if (at->attr_value->avseq_av.av_struct == NULL)
		return (NULLCP);

	return( (char *)at->attr_value->avseq_av.av_struct);
	
}

make_dsa_bind_arg (arg)
struct ds_bind_arg *arg;
{
#ifdef NEXT_VERSION
Entry my_entry;
char * passwd;

	arg->dba_version = DBA_VERSION_V1988;
	arg->dba_auth_type = DBA_AUTH_SIMPLE;
	arg->dba_time1 = NULLCP;
	arg->dba_time2 = NULLCP;

	if ((my_entry = local_find_entry (mydsadn ,TRUE)) == NULLENTRY) {
		arg->dba_dn = NULLDN;
		arg->dba_auth_type = DBA_AUTH_NONE;
		arg->dba_passwd[0] = 0;
		arg->dba_passwd_len = 0;
	} else {
		arg->dba_dn = dn_cpy(mydsadn);
		if ( (passwd = get_entry_passwd(my_entry->e_attributes)) != NULLCP) {
			(void) strncpy (arg->dba_passwd,passwd,DBA_MAX_PASSWD_LEN);
			arg->dba_passwd_len = strlen (passwd);
		} else {
			arg->dba_auth_type = DBA_AUTH_NONE;
			arg->dba_passwd[0] = 0;
			arg->dba_passwd_len = 0;
		}
	}
#else
	arg->dba_version = DBA_VERSION_V1988;
	arg->dba_auth_type = DBA_AUTH_SIMPLE;
	arg->dba_time1 = NULLCP;
	arg->dba_time2 = NULLCP;
	arg->dba_passwd[0] = 0;
	arg->dba_passwd_len = 0;
	arg->dba_dn = dn_cpy(mydsadn);
#endif
}

struct oper_act	* make_get_dsa_info_op(dn, di)
DN		  dn;
struct di_block	* di;
{
struct di_block	* di_tmp;
struct oper_act	* on_tmp;
struct ds_read_arg	* arg;

	DLOG(log_dsap, LLOG_TRACE, ("make_get_dsa_info_op"));

	if((on_tmp = oper_alloc()) == NULLOPER)
	{
		LLOG(log_dsap, LLOG_EXCEPTIONS, ("make_get_dsa_info_op - out of memory"));
		return(NULLOPER);
	}

	on_tmp->on_type = ON_TYPE_GET_DSA_INFO;
	on_tmp->on_arg = &(on_tmp->on_req);
	set_my_chain_args(&(on_tmp->on_req.dca_charg), dn);

	on_tmp->on_req.dca_dsarg.arg_type = OP_READ;
	arg = &(on_tmp->on_req.dca_dsarg.arg_rd);

	set_my_common_args(&(arg->rda_common));
 	arg->rda_common.ca_servicecontrol.svc_prio = SVC_PRIO_HIGH;

	arg->rda_object = dn_cpy(dn);			/* The important bit */
	arg->rda_eis.eis_allattributes = TRUE;
	arg->rda_eis.eis_select = NULLATTR;
	arg->rda_eis.eis_infotypes = EIS_ATTRIBUTESANDVALUES;

	on_tmp->on_dsas = di;
	for(di_tmp=di; di_tmp!=NULL_DI_BLOCK; di_tmp=di_tmp->di_next)
	{
	    di_tmp->di_type = DI_OPERATION;
	    di_tmp->di_oper = on_tmp;
	}

	return(on_tmp);
}

set_my_chain_args(cha, dn)
struct chain_arg	* cha;
DN dn;
{
	cha->cha_originator = dn_cpy(mydsadn);
	cha->cha_target = dn_cpy(dn);
	cha->cha_progress.op_resolution_phase = OP_PHASE_NOTSTARTED;
	cha->cha_progress.op_nextrdntoberesolved = OP_PHASE_NOTDEFINED;
	cha->cha_trace = NULLTRACEINFO;
	cha->cha_aliasderef = 0;
	cha->cha_aliasedrdns = 0;
	cha->cha_returnrefs = FALSE;
	cha->cha_reftype = RT_SUBORDINATE;
	cha->cha_domaininfo = NULLPE;
	cha->cha_timelimit = NULLCP;
}

set_my_common_args(ca)
struct common_args	* ca;
{
	ca->ca_servicecontrol.svc_options = SVC_OPT_PREFERCHAIN;
	ca->ca_servicecontrol.svc_prio = SVC_PRIO_HIGH;
	ca->ca_servicecontrol.svc_timelimit = SVC_NOTIMELIMIT;
	ca->ca_servicecontrol.svc_sizelimit = SVC_NOSIZELIMIT;
	ca->ca_servicecontrol.svc_scopeofreferral = SVC_REFSCOPE_NONE;
	ca->ca_requestor = dn_cpy(mydsadn);
	ca->ca_progress.op_resolution_phase = OP_PHASE_NOTSTARTED;
	ca->ca_progress.op_nextrdntoberesolved = OP_PHASE_NOTDEFINED;
	ca->ca_aliased_rdns = CA_NO_ALIASDEREFERENCED;
	ca->ca_security = (struct security_parms *) NULL;
	ca->ca_sig = (struct signature *) NULL;
	ca->ca_extensions = (struct extension *) NULL;
}

quipu_ctx_supported (ptr)
Entry ptr;
{
AV_Sequence avs;
Attr_Sequence as;
extern OID quipu_dsa_oid;
extern AttributeType at_applctx;
char dap_only = TRUE;		
char res = 1;
static OID dsp = NULLOID;
static OID quipu_dsp = NULLOID;

	/* return 0 if "ptr" is not a quipu DSA */
	/* return 1 if "ptr" represents a quipu_dsa (by objectclass) */
	/* return 2 if "ptr" represents a quipu_dsa with quipu context */
	/* return -1 if "ptr" represents a DAP only DSA */

	/* Should we use QuipuDSP to a non-Quipu DSA, if is claims
	 * to support it - currently implemented as "NO" ?
         */

	if (!check_in_oc (quipu_dsa_oid,ptr->e_oc))
		res = 0;  /* not a Quipu DSA */

	if (( as = entry_find_type (ptr,at_applctx)) == NULLATTR)
		return 1;

	if (dsp == NULLOID) {
		/* will both be null first time around... */
		dsp = oid_cpy (DIR_SYSTEM_AC);
		quipu_dsp = oid_cpy (DIR_QUIPU_AC);
	}

	for (avs=as->attr_value; avs != NULLAV; avs=avs->avseq_next) {
		if ((res != 0) && (oid_cmp ((OID)avs->avseq_av.av_struct, quipu_dsp) == 0 ))
			return 2;
		if (oid_cmp ((OID)avs->avseq_av.av_struct, dsp) == 0 )
			dap_only = FALSE;
	}

	if (dap_only)
		return -1;

	return res;
}


quipu_version_7(eptr)
Entry eptr;
{
char * p, *t, *s;
int res, vrsn;
char * v;

	/* return true is the string suggests quipu version 6.8 or more */

	/* Format of string is typically...
	 * quipu 6.8 #69 (trellis) of Thu Nov 15 15:58:24 GMT 1990
	 */

	if (!eptr || !eptr->e_dsainfo)
		return FALSE;

	if (((v = eptr->e_dsainfo->dsa_version) == NULLCP) ||
	    ((p = index (v,' ')) == NULLCP ) ||
	    ((t = index (p,'.')) == NULLCP ))
		return FALSE;

	if ((s = index (t,' ')) != NULLCP )
		*s = 0;

	*p++ = 0;
	*t++ = 0;

	vrsn = ( atoi (p) * 10 ) + atoi (t);

	if ((strcmp (v,"quipu") == 0) && (vrsn >= 68))
		res = TRUE;
	else
	    	res = FALSE;

	*--p = ' ';
	*--t = '.';

	if (s)
		*s = ' ';

	return res;
}
