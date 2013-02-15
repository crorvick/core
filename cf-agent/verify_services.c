/*
   Copyright (C) Cfengine AS

   This file is part of Cfengine 3 - written and maintained by Cfengine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of Cfengine, the applicable Commerical Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include "verify_services.h"

#include "verify_methods.h"
#include "constraints.h"
#include "promises.h"
#include "vars.h"
#include "attributes.h"
#include "cfstream.h"
#include "fncall.h"
#include "transaction.h"
#include "logging.h"

static int ServicesSanityChecks(Attributes a, Promise *pp);
static void SetServiceDefaults(Attributes *a);
static void DoVerifyServices(Attributes a, Promise *pp, const ReportContext *report_context);

/*****************************************************************************/

void VerifyServicesPromise(Promise *pp, const ReportContext *report_context)
{
    Attributes a = { {0} };

    a = GetServicesAttributes(pp);

    SetServiceDefaults(&a);

    if (ServicesSanityChecks(a, pp))
    {
        VerifyServices(a, pp, report_context);
    }
}

/*****************************************************************************/

static int ServicesSanityChecks(Attributes a, Promise *pp)
{
    Rlist *dep;

    switch (a.service.service_policy)
    {
    case cfsrv_start:
        break;

    case cfsrv_stop:
    case cfsrv_disable:
        if (strcmp(a.service.service_autostart_policy, "none") != 0)
        {
            CfOut(cf_error, "",
                  "!! Autostart policy of service promiser \"%s\" needs to be \"none\" when service policy is not \"start\", but is \"%s\"",
                  pp->promiser, a.service.service_autostart_policy);
            PromiseRef(cf_error, pp);
            return false;
        }
        break;

    default:
        CfOut(cf_error, "", "!! Invalid service policy for service \"%s\"", pp->promiser);
        PromiseRef(cf_error, pp);
        return false;
    }

    for (dep = a.service.service_depend; dep != NULL; dep = dep->next)
    {
        if (strcmp(pp->promiser, dep->item) == 0)
        {
            CfOut(cf_error, "", "!! Service promiser \"%s\" has itself as dependency", pp->promiser);
            PromiseRef(cf_error, pp);
            return false;
        }
    }

    if (a.service.service_type == NULL)
    {
        CfOut(cf_error, "", "!! Service type for service \"%s\" is not known", pp->promiser);
        PromiseRef(cf_error, pp);
        return false;
    }

#ifdef __MINGW32__

    if (strcmp(a.service.service_type, "windows") != 0)
    {
        CfOut(cf_error, "", "!! Service type for promiser \"%s\" must be \"windows\" on this system, but is \"%s\"",
              pp->promiser, a.service.service_type);
        PromiseRef(cf_error, pp);
        return false;
    }

#endif /* __MINGW32__ */

    return true;
}

/*****************************************************************************/

static void SetServiceDefaults(Attributes *a)
{
    if (a->service.service_autostart_policy == NULL)
    {
        a->service.service_autostart_policy = "none";
    }

    if (a->service.service_depend_chain == NULL)
    {
        a->service.service_depend_chain = "ignore";
    }

// default service type to "windows" on windows platforms
#ifdef __MINGW32__
    if (a->service.service_type == NULL)
    {
        a->service.service_type = "windows";
    }
#else
    if (a->service.service_type == NULL)
    {
        a->service.service_type = "bundle";
    }
#endif /* __MINGW32__ */
}

/*****************************************************************************/
/* Level                                                                     */
/*****************************************************************************/

void VerifyServices(Attributes a, Promise *pp, const ReportContext *report_context)
{
    CfLock thislock;

    // allow to start Cfengine windows executor without license
#ifdef __MINGW32__

    if ((LICENSES == 0) && (strcmp(WINSERVICE_NAME, pp->promiser) != 0))
    {
        return;
    }

#endif

    thislock = AcquireLock(pp->promiser, VUQNAME, CFSTARTTIME, a, pp, false);

    if (thislock.lock == NULL)
    {
        return;
    }

    NewScalar("this", "promiser", pp->promiser, DATA_TYPE_STRING);
    PromiseBanner(pp);

    if (strcmp(a.service.service_type, "windows") == 0)
    {
        VerifyWindowsService(a, pp);
    }
    else
    {
        DoVerifyServices(a, pp, report_context);
    }

    DeleteScalar("this", "promiser");
    YieldCurrentLock(thislock);
}

/*****************************************************************************/
/* Level                                                                     */
/*****************************************************************************/

static void DoVerifyServices(Attributes a, Promise *pp, const ReportContext *report_context)
{
    FnCall *default_bundle = NULL;
    Rlist *args = NULL;

// Need to set up the default service pack to eliminate syntax

    if (GetConstraintValue("service_bundle", pp, RVAL_TYPE_SCALAR) == NULL)
    {
        switch (a.service.service_policy)
        {
        case cfsrv_start:
            AppendRlist(&args, pp->promiser, RVAL_TYPE_SCALAR);
            AppendRlist(&args, "start", RVAL_TYPE_SCALAR);
            break;

        case cfsrv_restart:
            AppendRlist(&args, pp->promiser, RVAL_TYPE_SCALAR);
            AppendRlist(&args, "restart", RVAL_TYPE_SCALAR);
            break;

        case cfsrv_reload:
            AppendRlist(&args, pp->promiser, RVAL_TYPE_SCALAR);
            AppendRlist(&args, "restart", RVAL_TYPE_SCALAR);
            break;
            
        case cfsrv_stop:
        case cfsrv_disable:
        default:
            AppendRlist(&args, pp->promiser, RVAL_TYPE_SCALAR);
            AppendRlist(&args, "stop", RVAL_TYPE_SCALAR);
            break;

        }

        default_bundle = NewFnCall("default:standard_services", args);

        PromiseAppendConstraint(pp, "service_bundle", (Rval) {default_bundle, RVAL_TYPE_FNCALL }, "any", false);
        a.havebundle = true;
    }

// Set $(this.service_policy) for flexible bundle adaptation

    switch (a.service.service_policy)
    {
    case cfsrv_start:
        NewScalar("this", "service_policy", "start", DATA_TYPE_STRING);
        break;

    case cfsrv_restart:
        NewScalar("this", "service_policy", "restart", DATA_TYPE_STRING);
        break;

    case cfsrv_reload:
        NewScalar("this", "service_policy", "reload", DATA_TYPE_STRING);
        break;
        
    case cfsrv_stop:
    case cfsrv_disable:
    default:
        NewScalar("this", "service_policy", "stop", DATA_TYPE_STRING);
        break;
    }

    const Bundle *bp = PolicyGetBundle(PolicyFromPromise(pp), NULL, "agent", default_bundle->name);
    if (!bp)
    {
        bp = PolicyGetBundle(PolicyFromPromise(pp), NULL, "common", default_bundle->name);
    }

    if (default_bundle && bp == NULL)
    {
        cfPS(cf_inform, CF_FAIL, "", pp, a, " !! Service %s could not be invoked successfully\n", pp->promiser);
    }

    if (!DONTDO)
    {
        VerifyMethod("service_bundle", a, pp, report_context);  // Send list of classes to set privately?
    }
}

