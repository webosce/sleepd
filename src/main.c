// Copyright (c) 2011-2021 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

/**
 * @mainpage sleepd
 *
 * @section summary Summary
 *
 * Open webOS component to manage device suspend-resume cycles.
 *
 * @section description Description
 *
 * Sleepd is one of the important daemons started when webOS boots. It is
 * responsible for scheduling platform sleeps as soon as it is idle, so that we
 * see optimum battery performance. To achieve this it keeps polling on the
 * system to see if any of the other services or processes need the platform
 * running, and if not it sends the suspend message to all these components (so
 * that they can finish whatever they are doing ASAP and suspend). Sleepd then
 * lets the kernel know that the platform is ready to sleep. Once an interrupt
 * (such as key press) has woken the platform up, sleepd lets the entire system
 * know that the platform is up and running so that all the activities can
 * resume.
 *
 * Sleepd also manages the RTC alarms on the system by maintaining a SQlite
 * database for all the requested alarms.
 *
 * @section code-organization Code Organization
 *
 * The code for sleepd is organized into two main categories:
 *
 * - A bunch of individual power watcher modules which tie into the service bus
 *   and react to IPC messages passed in and/or which start their own threads
 *   and run separately.
 *
 * - A central module initialization system which ties them all together and
 *   handles all of the bookkeeping to keep them all running and gracefully shut
 *   them down when the sleepd service is asked to stop.
 *
 * Documentation for each of the power management modules is available in the
 * section to the left entitled "Modules".
 *
 * The modules each register themselves with the main initialization code using
 * a macro called {@link INIT_FUNC}.  It uses GCC-specific functionality to
 * run hook registration code when the binary is being loaded into memory.  As a
 * result, all of the code to register these hooks is run before {@link main()}
 * is called.  This creates a very modular code organizational approach in which
 * new power saving modules can be added independently of the main
 * initialization system.
 */

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <glib.h>
#include <pthread.h>
#include <stdbool.h>
#include <getopt.h>
#include <stdlib.h>
#include <json.h>
#include <luna-service2/lunaservice.h>

#include "init.h"
#include "logging.h"
#include "main.h"


static GMainLoop *mainloop = NULL;
static LSHandle *lsh = NULL, *webos_sh = NULL;

bool ChargerConnected(LSHandle *sh, LSMessage *message,
                      void *user_data); // defined in machine.c
bool ChargerStatus(LSHandle *sh, LSMessage *message,
                   void *user_data); // defined in machine.c

#define LOG_DOMAIN "SLEEPD-INIT: "

/**
 * Handle process signals asking us to terminate running of our service
 */
void
term_handler(int signal)
{
    g_main_loop_quit(mainloop);
}


GMainContext *
GetMainLoopContext(void)
{
    return g_main_loop_get_context(mainloop);
}

LSHandle *
GetLunaServiceHandle(void)
{
    return lsh;
}

GMainLoop *
GetMainLoop(void)
{
    return mainloop;
}

LSHandle *
GetWebosLunaServiceHandle(void)
{
    return webos_sh;
}

static nyx_device_handle_t nyxSystem = NULL;

nyx_device_handle_t
GetNyxSystemDevice(void)
{
    return nyxSystem;
}

static gboolean register_batteryd_status_cb(LSHandle *sh, const char *service,
        gboolean connected, void *ctx)
{
    LSError lserror;
    LSErrorInit(&lserror);
    bool retVal = true;

    if (connected)
    {
        /*
         * Register with com.webos.service.battery for events regarding changes in status
         * to the plug/unplug state of any chargers which may be attached to our
         * device.
         */
        retVal = LSCall(lsh, "luna://com.palm.lunabus/signal/addmatch",
                        "{\"category\":\"/\","
                        "\"method\":\"chargerConnected\"}", ChargerStatus, NULL, NULL, &lserror);

        if (retVal)
            /*
             * Now that we've got something listening for charger status changes,
             * request the current state of the charger from com.webos.service.battery
             */
            retVal = LSCall(lsh,
                            "luna://com.webos.service.battery/chargerStatusQuery",
                            "{}", ChargerStatus, NULL, NULL, &lserror);

        if (!retVal)
        {
            LSErrorPrint(&lserror, stderr);
            LSErrorFree(&lserror);
        }
    }

    return retVal;
}

/**
 * Main entry point for sleepd - runs the initialization hooks installed at program load time
 *
 * A bit counter-intuitively, this is not the first part of this program which
 * is run.
 *
 * First, everything which uses the {@link INIT_FUNC} macro in init.h are run,
 * which registers a bunch of hooks with the initialization system so that
 * individual modules can be registered without touching the main sleepd
 * initialization code.  Then, once all of those hooks are installed, execution
 * proceeds to this function which actually runs those hooks.
 *
 * - Initializes sleepd.
 * - Attaches as a Luna service under com.palm.sleep.
 * - Attaches to Nyx.
 * - Subscribes to events related to the charger being plugged and unplugged from the com.palm.power service.
 * - Calls {@link TheOneInit()} to finish initialization of the service.
 * - Issues a request to the com.webos.service.battery service to check on the plugged/unplugged status of the charger.
 *
 * @param   argc        Number of command-line arguments.
 * @param   argv        List of command-line arguments.
 *
 * @todo Move the logging initialization functionality into {@link TheOneInit()}.
 */
int
main(int argc, char **argv)
{
    bool retVal;
    int ret = -1;
    /*
     * Register a function to be able to gracefully handle termination signals
     * from the OS or other processes.
     */
    signal(SIGTERM, term_handler);
    signal(SIGINT, term_handler);

    mainloop = g_main_loop_new(NULL, FALSE);

    /*
     *  initialize the lunaservice and we want it before all the init
     *  stuff happening.
     */
    LSError lserror;
    LSErrorInit(&lserror);

    /*
     * Register ourselves as the "com.webos.service.power" service.
     */
    retVal = LSRegister("com.webos.service.power", &webos_sh, &lserror);

    if (retVal)
    {
        retVal = LSGmainAttach(webos_sh, mainloop, &lserror);

    }
    else
    {
        SLEEPDLOG_CRITICAL(MSGID_SRVC_REGISTER_FAIL, 1, PMLOGKS(ERRTEXT,
                           lserror.message), "Could not initialize com.webos.service.power");
        LSErrorFree(&lserror);
        goto error;
    }

    /*
     * Register ourselves as the original "com.palm.sleep" service ( to be deprecated soon)
     */
    retVal = LSRegister("com.palm.sleep", &lsh, &lserror);

    if (retVal)
    {
        retVal = LSGmainAttach(lsh, mainloop, &lserror);

    }
    else
    {
        SLEEPDLOG_CRITICAL(MSGID_SRVC_REGISTER_FAIL, 1, PMLOGKS(ERRTEXT,
                           lserror.message), "Could not initialize com.palm.sleep");
        LSErrorFree(&lserror);
        goto error;
    }

    if (LSRegisterServerStatusEx(lsh, "com.webos.service.battery",
                                 (LSServerStatusFunc)register_batteryd_status_cb, NULL, NULL, &lserror) == false)
    {
        LSErrorFree(&lserror);
        goto error;
    }

    /*
     * Connect to Nyx so we can use it later.
     */
    ret = nyx_device_open(NYX_DEVICE_SYSTEM, "Main", &nyxSystem);

    if (ret != NYX_ERROR_NONE)
    {
        SLEEPDLOG_CRITICAL(MSGID_NYX_DEVICE_OPEN_FAIL, 1,
                           PMLOGKS(CAUSE, "Unable to open the nyx device system"), "");
        abort();
    }

    /*
     * Call our main initialization function - this is the function which
     * is supposed to handle initializing pretty much everything for us.
     */
    TheOneInit();

    SLEEPDLOG_DEBUG("Sleepd daemon started");

    g_main_loop_run(mainloop);

error:
    g_main_loop_unref(mainloop);
    return 0;
}
