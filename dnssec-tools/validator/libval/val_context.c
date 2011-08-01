/*
 * Copyright 2005-2011 SPARTA, Inc.  All rights reserved.
 * See the COPYING file distributed with this software for details.
 */
/*
 * DESCRIPTION
 * Contains routines for context creation/deletion
 */ 
#include "validator-config.h"
#include "validator-internal.h"

#include "val_support.h"
#include "val_policy.h"
#include "val_cache.h"
#include "val_assertion.h"
#include "val_context.h"

#define GET_LATEST_TIMESTAMP(ctx, file, cur_ts, new_ts) do { \
    memset(&new_ts, 0, sizeof(struct stat));\
    if (!file) {\
        if (cur_ts != 0) {\
            val_log(ctx, LOG_WARNING, "val_resolve_and_check(): %s missing; trying to operate without it.", file);\
        }\
    } else {\
        if(0 != stat(file, &new_ts)) {\
            val_log(ctx, LOG_WARNING, "val_resolve_and_check(): %s missing; trying to operate without it.", file);\
        }\
    }\
}while (0)


static val_context_t *the_default_context = NULL;

#ifdef WIN32
static int wsaInitialized = 0;
WSADATA wsaData;
#endif

#ifndef VAL_NO_THREADS

pthread_mutex_t ctx_default =  PTHREAD_MUTEX_INITIALIZER;
#define LOCK_DEFAULT_CONTEXT() \
    pthread_mutex_lock(&ctx_default)
#define UNLOCK_DEFAULT_CONTEXT() \
    pthread_mutex_unlock(&ctx_default)

#else
#define LOCK_DEFAULT_CONTEXT()
#define UNLOCK_DEFAULT_CONTEXT()
#endif

/*
 * re-read resolver policy into the context
 */
static int 
val_refresh_resolver_policy(val_context_t * context)
{
    if (context == NULL) 
        return VAL_NO_ERROR;

    if (read_res_config_file(context) != VAL_NO_ERROR) {
        context->r_timestamp = -1;
        val_log(context, LOG_WARNING, 
                "val_refresh_resolver_policy(): Resolver configuration could not be read; using older values");
    }
    return VAL_NO_ERROR; 
}


/*
 * re-read validator policy into the context
 */
static int 
val_refresh_validator_policy(val_context_t * context)
{
    struct dnsval_list *dnsval_l;
    int is_override = 0;
    val_context_t *old_default_context = NULL;
    if (context == NULL) 
        return VAL_NO_ERROR;

    if (read_val_config_file(context, context->label, &is_override) != VAL_NO_ERROR) {
        for(dnsval_l = context->dnsval_l; dnsval_l; dnsval_l=dnsval_l->next)
            dnsval_l->v_timestamp = -1;
        val_log(context, LOG_WARNING, 
                "val_refresh_validator_policy(): Validator configuration could not be read; using older values");
    }

    LOCK_DEFAULT_CONTEXT();
    if (is_override && (the_default_context != context)) {
        old_default_context = the_default_context;
        the_default_context = context;
    }
    UNLOCK_DEFAULT_CONTEXT();
    
    /* we've changed the default context, free the old one */
    if (old_default_context) {
        val_free_context(old_default_context);
    }

    return VAL_NO_ERROR; 
}

/*
 * re-read root.hints policy into the context
 */
static int 
val_refresh_root_hints(val_context_t * context)
{
    if (context == NULL)
        return VAL_NO_ERROR;

    if (read_root_hints_file(context) != VAL_NO_ERROR) {
        context->h_timestamp = -1;
        val_log(context, LOG_WARNING, 
                "val_refresh_root_hints(): Root Hints could not be read; using older values");
    }

    return VAL_NO_ERROR;
}
/*
 * Function: val_refresh_context
 *
 * Purpose:   set up context for a query
 *
 * Parameter: results -- results for query
 *            ctx -- user supplied context, if any
 *
 * Returns:   VAL_NO_ERROR or error code to return to user
 *
 */
static int
val_refresh_context(val_context_t *context)
{
    struct stat rsb, vsb, hsb;
    struct dnsval_list *dnsval_l;
    int retval;

    if (NULL == context)
        return VAL_BAD_ARGUMENT;

    CTX_LOCK_POL_SH(context);

    GET_LATEST_TIMESTAMP(context, context->resolv_conf, context->r_timestamp,
                         rsb);
    if (rsb.st_mtime != 0 &&  rsb.st_mtime != context->r_timestamp) {
        if (VAL_NO_ERROR != (retval = val_refresh_resolver_policy(context))) {
            goto err;
        }
    }    
    GET_LATEST_TIMESTAMP(context, context->root_conf, context->h_timestamp, hsb);
    if (hsb.st_mtime != 0 &&  hsb.st_mtime != context->h_timestamp){
        if (VAL_NO_ERROR != (retval = val_refresh_root_hints(context))) {
            return retval;
        }
    }

    /* dnsval.conf can point to a list of files */
    for (dnsval_l = context->dnsval_l; dnsval_l; dnsval_l=dnsval_l->next) {
        GET_LATEST_TIMESTAMP(context,  dnsval_l->dnsval_conf, 
                             dnsval_l->v_timestamp, vsb);
        if (vsb.st_mtime != 0 &&  vsb.st_mtime != dnsval_l->v_timestamp) {
            retval = val_refresh_validator_policy(context);
            if (VAL_NO_ERROR != retval) {
                goto err;
            }
            break;
        }
    }

    CTX_UNLOCK_POL(context); 

    return VAL_NO_ERROR;

err:
    CTX_UNLOCK_POL(context); 
    return retval;

}

/*
 * Create a context with given configuration files
 */
int
val_create_context_with_conf(char *label, 
                             char *dnsval_conf, 
                             char *resolv_conf, 
                             char *root_conf, 
                             val_context_t ** newcontext)
{
    int             retval;
    char *base_dnsval_conf = NULL;
    int is_override = 0;
    val_context_t *old_default_context = NULL;

#ifdef WIN32 
    if (!wsaInitialized) {
        wsaInitialized = 1;
        if (0 != WSAStartup(0x202, &wsaData)) {
            return VAL_INTERNAL_ERROR;
        }
    }
#endif


    if (newcontext == NULL)
        return VAL_BAD_ARGUMENT;

    LOCK_DEFAULT_CONTEXT();

    /* Check if the request is for the default context, and we have one available */
    /* 
     *  either label should be NULL, or if label is not NULL, our global policy should
     *  be set so that environment overrides what ever is passed by the app
     */
    if (the_default_context && 
        (label == NULL || 
         (the_default_context->g_opt && 
          (the_default_context->g_opt->env_policy == VAL_POL_GOPT_OVERRIDE || 
           the_default_context->g_opt->app_policy == VAL_POL_GOPT_OVERRIDE)))) {

        *newcontext = the_default_context;
        UNLOCK_DEFAULT_CONTEXT();

        /* have configuration files changed? */
        if (VAL_NO_ERROR != (retval = val_refresh_context(*newcontext))) {
            return retval;
        }

        val_log(*newcontext, LOG_INFO, "reusing default context");
        return retval;
    }
    UNLOCK_DEFAULT_CONTEXT();

    *newcontext = (val_context_t *) MALLOC(sizeof(val_context_t));
    if (*newcontext == NULL) {
        return VAL_OUT_OF_MEMORY;
    }
    memset(*newcontext, 0, sizeof(val_context_t));

#ifndef VAL_NO_THREADS
    if (0 != pthread_rwlock_init(&(*newcontext)->pol_rwlock, NULL)) {
        FREE(*newcontext);
        *newcontext = NULL;
        return VAL_INTERNAL_ERROR;
    }
    if (0 != pthread_mutex_init(&(*newcontext)->ac_lock, NULL)) {
        pthread_rwlock_destroy(&(*newcontext)->pol_rwlock);
        FREE(*newcontext);
        *newcontext = NULL;
        return VAL_INTERNAL_ERROR;
    }
#endif

    if (snprintf
        ((*newcontext)->id, VAL_CTX_IDLEN - 1, "%u",
         (unsigned) (*newcontext)) < 0)
        strcpy((*newcontext)->id, "libval");

    CTX_LOCK_POL_SH((*newcontext));

    /* 
     * Set default configuration files 
     */
    (*newcontext)->resolv_conf = resolv_conf? strdup(resolv_conf) : resolv_conf_get(); 
    (*newcontext)->r_timestamp = 0;
    (*newcontext)->root_conf = root_conf? strdup(root_conf) : root_hints_get(); 
    (*newcontext)->h_timestamp = 0;

    (*newcontext)->root_ns = NULL; 
    (*newcontext)->nslist = NULL; 

    (*newcontext)->e_pol =
        (policy_entry_t **) MALLOC(MAX_POL_TOKEN * sizeof(policy_entry_t *));
    if ((*newcontext)->e_pol == NULL) {
        retval = VAL_OUT_OF_MEMORY;
        goto err;
    }
    memset(((*newcontext)->e_pol), 0,
           MAX_POL_TOKEN * sizeof(policy_entry_t *));
   
    /*
     * Read the Root Hints file; has to be read before resolver config file 
     */
    if ((retval = read_root_hints_file(*newcontext)) != VAL_NO_ERROR) {
        goto err;
    }

    /* set the log targets to NULL */
    (*newcontext)->val_log_targets = NULL;

    /*
     * Read the Resolver configuration file 
     */
    if ((retval = read_res_config_file(*newcontext)) != VAL_NO_ERROR) {
        goto err;
    }

    /*
     * Read the validator configuration file 
     */
    (*newcontext)->q_list = NULL;
    base_dnsval_conf = dnsval_conf? strdup(dnsval_conf) : dnsval_conf_get();
    if (base_dnsval_conf == NULL) {
        val_log(*newcontext, LOG_ERR, "val_create_context_with_conf(): No dnsval.conf file configured");
        retval = VAL_CONF_NOT_FOUND;
        goto err;
    }

    /* Add a single node in the dnsval_list structure */
    if (NULL == ((*newcontext)->dnsval_l = 
                (struct dnsval_list *) MALLOC (sizeof(struct dnsval_list))))  {
        FREE(base_dnsval_conf);
        retval = VAL_OUT_OF_MEMORY;
        goto err;
    }
    (*newcontext)->dnsval_l->dnsval_conf = base_dnsval_conf; 
    (*newcontext)->dnsval_l->v_timestamp = 0;
    (*newcontext)->dnsval_l->next = NULL;
    
    if ((retval =
         read_val_config_file(*newcontext, label, &is_override)) != VAL_NO_ERROR) {
        goto err;
    }

    (*newcontext)->default_qflags = 0; 
    if ((*newcontext)->val_log_targets != NULL) {
        (*newcontext)->default_qflags |= VAL_QUERY_AC_DETAIL;
    }

    CTX_UNLOCK_POL((*newcontext));

    val_log(*newcontext, LOG_DEBUG, 
            "val_create_context_with_conf(): Context created with %s %s %s", 
            (*newcontext)->dnsval_l->dnsval_conf,
            (*newcontext)->resolv_conf,
            (*newcontext)->root_conf);

    LOCK_DEFAULT_CONTEXT();
    if (label == NULL || is_override) {
        old_default_context = the_default_context;
        the_default_context = *newcontext;
    }
    UNLOCK_DEFAULT_CONTEXT();
    
    if (old_default_context) {
        val_free_context(old_default_context);
    }
    return VAL_NO_ERROR;

err:
    CTX_UNLOCK_POL((*newcontext));
    val_free_context(*newcontext);
    *newcontext = NULL;
    return retval;
}

/*
 * Create a context with default configuration files
 */
int
val_create_context(char *label, 
                   val_context_t ** newcontext)
{
    return val_create_context_with_conf(label, NULL, NULL, NULL, newcontext);
}

/*
 * Function: val_create_or_refresh_context
 *
 * Purpose:   set up context for a query
 *
 * Parameter: results -- results for query
 *            ctx -- user supplied context, if any
 *
 * Returns:   VAL_NO_ERROR or error code to return to user
 *
 */
val_context_t *
val_create_or_refresh_context(val_context_t *ctx)
{
    val_context_t *context = NULL;
    int retval;

    /*
     * Create a default context if one does not exist
     */
    if (ctx == NULL) {
        if (VAL_NO_ERROR != (retval = val_create_context(NULL, &context)))
            return NULL;
    } else {
        /* have configuration files changed? */
        context = ctx;
        if (VAL_NO_ERROR != (retval = val_refresh_context(context))) {
            return NULL;
        }
    }

    if (context)
        CTX_LOCK_POL_SH(context);

    return context;
}

/*
 * Release memory associated with context
 */
void
val_free_context(val_context_t * context)
{
    struct val_query_chain *q;
    val_context_t *ctx = context;

    /*
     * Never free the default context 
     */
    LOCK_DEFAULT_CONTEXT();
    if (ctx == the_default_context) {
        CTX_UNLOCK_POL(ctx);
        ctx = NULL;
    }
    UNLOCK_DEFAULT_CONTEXT();

    if (ctx == NULL)
        return;

#ifndef VAL_NO_THREADS
    CTX_UNLOCK_POL(context);
    CTX_LOCK_POL_EX(context);
    pthread_rwlock_destroy(&context->pol_rwlock);
    pthread_mutex_destroy(&context->ac_lock);
#endif

    if (context->label)
        FREE(context->label);

    if (context->search)
        FREE(context->search);

    if (context->resolv_conf)
        FREE(context->resolv_conf);

    if (context->root_conf)
        FREE(context->root_conf);

    if (context->root_ns)
        free_name_servers(&context->root_ns);
    
    destroy_respol(context);
    destroy_valpol(context);
    FREE(context->e_pol);

    while (NULL != (q = context->q_list)) {
        context->q_list = q->qc_next;
        free_query_chain_structure(q);
        q = NULL;
    }

#ifndef VAL_NO_ASYNC
    {
        val_async_status  *as;
        while (NULL != context->as_list) {
            as = context->as_list;
            context->as_list = as->val_as_next;
            as->val_as_next = NULL;
            as->val_as_ctx = NULL;
            val_async_status_free(as);
        }
    }
#endif

    FREE(context);
}

#ifndef VAL_NO_ASYNC
/* remove asynchronous status from context async queries list */
int
val_context_as_remove(val_context_t *context, val_async_status *as)
{
    val_async_status *prev, *curr = NULL;

    if ((NULL == context) || (NULL == as) ||
        (as->val_as_ctx && (as->val_as_ctx != context)))
        return VAL_BAD_ARGUMENT;

    if (NULL == context->as_list)
        return VAL_NO_ERROR;

    if (as == context->as_list) {
        curr = context->as_list;
        context->as_list = as->val_as_next;
    } else {
        prev = context->as_list;
        curr = prev->val_as_next;
        for (; curr; prev = curr, curr = curr->val_as_next) {
            if (curr != as)
                continue;
            prev->val_as_next = curr->val_as_next;
            break;
        } /* for */
    } /* as->val_as_ctx */

    if (curr) {
        curr->val_as_next = NULL;
        curr->val_as_ctx = NULL;
    }
    return VAL_NO_ERROR;
}
#endif

/*
 * Free all internal state associated with the validator
 * There should be no active contexts when this function
 * is invoked.
 * Only used when testing if we have memory leaks
 */
int
val_free_validator_state()
{
    val_context_t * saved_ctx = NULL;

    free_validator_cache();

    LOCK_DEFAULT_CONTEXT();
    if (the_default_context != NULL) {
        /*
         * must clear the_default_context to prevent deadlock
         * in val_free_context.
         */
        saved_ctx = the_default_context;
        the_default_context = NULL;
    }
    UNLOCK_DEFAULT_CONTEXT();

    if (saved_ctx)
        val_free_context(saved_ctx);

#ifdef WIN32
    WSACleanup();
#endif

    return VAL_NO_ERROR;
}

