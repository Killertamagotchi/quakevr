/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2020-2021 Vittorio Romeo

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// host.c -- coordinates spawning and killing of local servers

#include "host.hpp"
#include "quakedef.hpp"
#include "bgmusic.hpp"
#include "vr.hpp"
#include "vr_cvars.hpp"
#include "cmd.hpp"
#include "common.hpp"
#include "cdaudio.hpp"
#include "quakeparms.hpp"
#include "sbar.hpp"
#include "wad.hpp"
#include "net.hpp"
#include "glquake.hpp"
#include "menu.hpp"
#include "keys.hpp"
#include "protocol.hpp"
#include "msg.hpp"
#include "client.hpp"
#include "console.hpp"
#include "saveutil.hpp"
#include "sys.hpp"
#include "server.hpp"
#include "screen.hpp"
#include "gl_texmgr.hpp"
#include "vid.hpp"
#include "draw.hpp"
#include "q_sound.hpp"
#include "input.hpp"
#include "view.hpp"
#include "developer.hpp"
#include "qcvm.hpp"

#include <csetjmp>
#include <exception>

/*

A server can allways be started, even if the system started out as a client
to a remote system.

A client can NOT be started if the system started as a dedicated server.

Memory is cleared / released when a server or client begins, not when they end.

*/

quakeparms_t* host_parms;

bool host_initialized; // true if into command execution

double host_frametime;
double realtime;    // without any filtering or bounding
double oldrealtime; // last frame run

int host_framecount;

int host_hunklevel;

int minimum_memory;

client_t* host_client; // current client

jmp_buf host_abortserver;
bool host_abortserver_setjmp_done{false};

byte* host_colormap;
float host_netinterval;

cvar_t host_framerate = {
    "host_framerate", "0", CVAR_NONE};                // set for slow motion
cvar_t host_speeds = {"host_speeds", "0", CVAR_NONE}; // set for running times
cvar_t host_maxfps = {"host_maxfps", "72", CVAR_ARCHIVE};   // johnfitz
cvar_t host_timescale = {"host_timescale", "0", CVAR_NONE}; // johnfitz
cvar_t max_edicts = {
    "max_edicts", "15000", CVAR_NONE}; // johnfitz //ericw -- changed from 2048
                                       // to 8192, removed CVAR_ARCHIVE

// QSS
cvar_t cl_nocsqc = {"cl_nocsqc", "0",
    CVAR_NONE}; // spike -- blocks the loading of any csqc modules

cvar_t sys_ticrate = {"sys_ticrate", "0.05", CVAR_NONE}; // dedicated server
cvar_t serverprofile = {"serverprofile", "0", CVAR_NONE};

cvar_t fraglimit = {"fraglimit", "0", CVAR_NOTIFY | CVAR_SERVERINFO};
cvar_t timelimit = {"timelimit", "0", CVAR_NOTIFY | CVAR_SERVERINFO};
cvar_t teamplay = {"teamplay", "0", CVAR_NOTIFY | CVAR_SERVERINFO};
cvar_t samelevel = {"samelevel", "0", CVAR_NONE};
cvar_t noexit = {"noexit", "0", CVAR_NOTIFY | CVAR_SERVERINFO};
cvar_t skill = {"skill", "1", CVAR_NONE};           // 0 - 3
cvar_t deathmatch = {"deathmatch", "0", CVAR_NONE}; // 0, 1, or 2
cvar_t coop = {"coop", "0", CVAR_NONE};             // 0 or 1

cvar_t pausable = {"pausable", "1", CVAR_NONE};

cvar_t developer = {"developer", "0", CVAR_NONE};

cvar_t temp1 = {"temp1", "0", CVAR_NONE};

cvar_t devstats = {"devstats", "0",
    CVAR_NONE}; // johnfitz -- track developer statistics that vary every frame

devstats_t dev_stats, dev_peakstats;
overflowtimes_t
    dev_overflows; // this stores the last time overflow messages were
                   // displayed, not the last time overflows occured

/*
================
Max_Edicts_f -- johnfitz
================
*/
static void Max_Edicts_f(cvar_t* var)
{
    (void)var;

    // TODO: clamp it here?
    if(cls.state == ca_connected || sv.active)
    {
        Con_Printf(
            "Changes to max_edicts will not take effect until the next time a "
            "map is loaded.\n");
    }
}

/*
================
Max_Fps_f -- ericw
================
*/
static void Max_Fps_f(cvar_t* var)
{
    // QSS
    if(var->value > 72 || var->value <= 0)
    {
        if(!host_netinterval)
        {
            Con_Printf("Using renderer/network isolation.\n");
        }
        host_netinterval = 1.0 / 72;
    }
    else
    {
        if(host_netinterval)
        {
            Con_Printf("Disabling renderer/network isolation.\n");
        }
        host_netinterval = 0;

        if(var->value > 72)
        {
            Con_Warning("host_maxfps above 72 breaks physics.\n");
        }
    }
}

/*
================
Host_EndGame
================
*/
void Host_EndGame(const char* message, ...)
{
    va_list argptr;
    char string[1024];

    va_start(argptr, message);
    q_vsnprintf(string, sizeof(string), message, argptr);
    va_end(argptr);
    Con_DPrintf("Host_EndGame: %s\n", string);

    PR_SwitchQCVM(nullptr);

    if(sv.active)
    {
        Host_ShutdownServer(false);
    }

    if(cls.state == ca_dedicated)
    {
        Sys_Error("Host_EndGame: %s\n", string); // dedicated servers exit
    }

    if(cls.demonum != -1)
    {
        CL_NextDemo();
    }
    else
    {
        CL_Disconnect();
    }

    if(!host_abortserver_setjmp_done)
    {
        std::terminate();
    }

    longjmp(host_abortserver, 1);
}

/*
================
Host_Warn

Prints a warning without shutting down
================
*/
void Host_Warn(const char* error, ...)
{
    va_list argptr;
    char string[1024];
    static bool inwarn = false;

    if(inwarn)
    {
        Sys_Error("Host_Warn: recursively entered");
    }
    inwarn = true;

    SCR_EndLoadingPlaque(); // reenable screen updates

    va_start(argptr, error);
    q_vsnprintf(string, sizeof(string), error, argptr);
    va_end(argptr);
    Con_Printf("Host_Warn: %s\n", string);

    inwarn = false;
}

/*
================
Host_Error

This shuts down both the client and server
================
*/
void Host_Error(const char* error, ...)
{
    va_list argptr;
    char string[1024];
    static bool inerror = false;

    if(inerror)
    {
        Sys_Error("Host_Error: recursively entered");
    }
    inerror = true;

    if(cl.qcvm.progs)
    {
        glDisable(GL_SCISSOR_TEST); // equivelent to drawresetcliparea, to reset
                                    // any damage if we crashed in csqc.
    }

    PR_SwitchQCVM(nullptr);

    SCR_EndLoadingPlaque(); // reenable screen updates

    va_start(argptr, error);
    q_vsnprintf(string, sizeof(string), error, argptr);
    va_end(argptr);
    Con_Printf("Host_Error: %s\n", string);

    // QSS
    Con_Redirect(nullptr);

    if(sv.active)
    {
        Host_ShutdownServer(false);
    }

    if(cls.state == ca_dedicated)
    {
        Sys_Error("Host_Error: %s\n", string); // dedicated servers exit
    }

    CL_Disconnect();
    cls.demonum = -1;
    cl.intermission = 0; // johnfitz -- for errors during intermissions
                         // (changelevel with no map found, etc.)

    inerror = false;

    if(!host_abortserver_setjmp_done)
    {
        std::terminate();
    }

    longjmp(host_abortserver, 1);
}

/*
================
Host_FindMaxClients
================
*/
void Host_FindMaxClients()
{
    int i;

    svs.maxclients = 1;

    i = COM_CheckParm("-dedicated");
    if(i)
    {
        cls.state = ca_dedicated;
        if(i != (com_argc - 1))
        {
            svs.maxclients = Q_atoi(com_argv[i + 1]);
        }
        else
        {
            svs.maxclients = 8;
        }
    }
    else
    {
        cls.state = ca_disconnected;
    }

    i = COM_CheckParm("-listen");
    if(i)
    {
        if(cls.state == ca_dedicated)
        {
            Sys_Error("Only one of -dedicated or -listen can be specified");
        }
        if(i != (com_argc - 1))
        {
            svs.maxclients = Q_atoi(com_argv[i + 1]);
        }
        else
        {
            svs.maxclients = 8;
        }
    }
    if(svs.maxclients < 1)
    {
        svs.maxclients = 8;
    }
    else if(svs.maxclients > MAX_SCOREBOARD)
    {
        svs.maxclients = MAX_SCOREBOARD;
    }

    svs.maxclientslimit = svs.maxclients;
    if(svs.maxclientslimit < 16)
    {
        svs.maxclientslimit = 16;
    }
    svs.clients = (struct client_t*)Hunk_AllocName(
        svs.maxclientslimit * sizeof(client_t), "clients");

    if(svs.maxclients > 1)
    {
        Cvar_SetQuick(&deathmatch, "1");
    }
    else
    {
        Cvar_SetQuick(&deathmatch, "0");
    }
}

void Host_Version_f()
{
    Con_Printf("Quake Version %1.2f\n", VERSION);
    Con_Printf("QuakeSpasm Version " QUAKESPASM_VER_STRING "\n");
    Con_Printf("Exe: " __TIME__ " " __DATE__ "\n");
}

/* cvar callback functions : */
void Host_Callback_Notify(cvar_t* var)
{
    if(sv.active)
    {
        SV_BroadcastPrintf(
            "\"%s\" changed to \"%s\"\n", var->name, var->string);
    }
}

/*
=======================
Host_InitLocal
======================
*/
void Host_InitLocal()
{
    Cmd_AddCommand("version", Host_Version_f);

    Host_InitCommands();

    Cvar_RegisterVariable(&host_framerate);
    Cvar_RegisterVariable(&host_speeds);
    Cvar_RegisterVariable(&host_maxfps); // johnfitz
    Cvar_SetCallback(&host_maxfps, Max_Fps_f);
    Cvar_RegisterVariable(&host_timescale); // johnfitz

    Cvar_RegisterVariable(&cl_nocsqc);  // spike // QSS
    Cvar_RegisterVariable(&max_edicts); // johnfitz
    Cvar_SetCallback(&max_edicts, Max_Edicts_f);
    Cvar_RegisterVariable(&devstats); // johnfitz

    Cvar_RegisterVariable(&sys_ticrate);
    Cvar_RegisterVariable(&sys_throttle);
    Cvar_RegisterVariable(&serverprofile);

    Cvar_RegisterVariable(&fraglimit);
    Cvar_RegisterVariable(&timelimit);
    Cvar_RegisterVariable(&teamplay);
    Cvar_SetCallback(&fraglimit, Host_Callback_Notify);
    Cvar_SetCallback(&timelimit, Host_Callback_Notify);
    Cvar_SetCallback(&teamplay, Host_Callback_Notify);
    Cvar_RegisterVariable(&samelevel);
    Cvar_RegisterVariable(&noexit);
    Cvar_SetCallback(&noexit, Host_Callback_Notify);
    Cvar_RegisterVariable(&skill);
    Cvar_RegisterVariable(&developer);
    Cvar_RegisterVariable(&coop);
    Cvar_RegisterVariable(&deathmatch);

    Cvar_RegisterVariable(&pausable);

    Cvar_RegisterVariable(&temp1);

    Host_FindMaxClients();
}


/*
===============
Host_WriteConfiguration

Writes key bindings and archived cvars to config.cfg
===============
*/
void Host_WriteConfiguration()
{
    FILE* f;

    // dedicated servers initialize the host but don't parse and set the
    // config.cfg cvars
    if(host_initialized && !isDedicated && !host_parms->errstate)
    {
        f = fopen(va("%s/config.cfg", com_gamedir), "w");
        if(!f)
        {
            Con_Printf("Couldn't write config.cfg.\n");
            return;
        }

        // VID_SyncCvars (); //johnfitz -- write actual current mode to config
        // file, in case cvars were messed with

        Key_WriteBindings(f);
        Cvar_WriteVariables(f);

        // johnfitz -- extra commands to preserve state
        fprintf(f, "vid_restart\n");
        if(in_mlook.state & 1)
        {
            fprintf(f, "+mlook\n");
        }
        // johnfitz

        fclose(f);
    }
}


/*
=================
SV_ClientPrintf

Sends text across to be displayed
FIXME: make this just a stuffed echo?
=================
*/
void SV_ClientPrintf(const char* fmt, ...)
{
    va_list argptr;
    char string[1024];

    va_start(argptr, fmt);
    q_vsnprintf(string, sizeof(string), fmt, argptr);
    va_end(argptr);

    MSG_WriteByte(&host_client->message, svc_print);
    MSG_WriteString(&host_client->message, string);
}

/*
=================
SV_BroadcastPrintf

Sends text to all active clients
=================
*/
void SV_BroadcastPrintf(const char* fmt, ...)
{
    va_list argptr;
    char string[1024];
    int i;

    va_start(argptr, fmt);
    q_vsnprintf(string, sizeof(string), fmt, argptr);
    va_end(argptr);

    for(i = 0; i < svs.maxclients; i++)
    {
        if(svs.clients[i].active && svs.clients[i].spawned)
        {
            MSG_WriteByte(&svs.clients[i].message, svc_print);
            MSG_WriteString(&svs.clients[i].message, string);
        }
    }
}

/*
=================
Host_ClientCommands

Send text over to the client to be executed
=================
*/
void Host_ClientCommands(const char* fmt, ...)
{
    va_list argptr;
    char string[1024];

    va_start(argptr, fmt);
    q_vsnprintf(string, sizeof(string), fmt, argptr);
    va_end(argptr);

    MSG_WriteByte(&host_client->message, svc_stufftext);
    MSG_WriteString(&host_client->message, string);
}

/*
=====================
SV_DropClient

Called when the player is getting totally kicked off the host
if (crash = true), don't bother sending signofs
=====================
*/
void SV_DropClient(bool crash)
{
    int saveSelf;
    int i;
    client_t* client;

    if(!crash)
    {
        // send any final messages (don't check for errors)
        if(NET_CanSendMessage(host_client->netconnection))
        {
            MSG_WriteByte(&host_client->message, svc_disconnect);
            NET_SendMessage(host_client->netconnection, &host_client->message);
        }

        if(host_client->edict && host_client->spawned)
        {
            // call the prog function for removing a client
            // this will set the body to a dead frame, among other things
            qcvm_t* oldvm = qcvm;
            PR_SwitchQCVM(nullptr);
            PR_SwitchQCVM(&sv.qcvm);

            saveSelf = pr_global_struct->self;
            pr_global_struct->self = EDICT_TO_PROG(host_client->edict);
            PR_ExecuteProgram(pr_global_struct->ClientDisconnect);
            pr_global_struct->self = saveSelf;

            PR_SwitchQCVM(nullptr);
            PR_SwitchQCVM(oldvm);
        }

        Sys_Printf("Client %s removed\n", host_client->name);
    }

    // break the net connection
    NET_Close(host_client->netconnection);
    host_client->netconnection = nullptr;

    SVFTE_DestroyFrames(host_client); // release any delta state

    // free the client (the body stays around)
    host_client->active = false;
    host_client->name[0] = 0;
    host_client->old_frags = -999999;
    net_activeconnections--;

    if(host_client->download.file)
    {
        fclose(host_client->download.file);
    }

    memset(&host_client->download, 0, sizeof(host_client->download));

    // send notification to all clients
    for(i = 0, client = svs.clients; i < svs.maxclients; i++, client++)
    {
        if(!client->knowntoqc)
        {
            continue;
        }

        MSG_WriteByte(&client->message, svc_updatename);
        MSG_WriteByte(&client->message, host_client - svs.clients);
        MSG_WriteString(&client->message, "");
        MSG_WriteByte(&client->message, svc_updatefrags);
        MSG_WriteByte(&client->message, host_client - svs.clients);
        MSG_WriteShort(&client->message, 0);
        MSG_WriteByte(&client->message, svc_updatecolors);
        MSG_WriteByte(&client->message, host_client - svs.clients);
        MSG_WriteByte(&client->message, 0);
    }
}

/*
==================
Host_ShutdownServer

This only happens at the end of a game, not between levels
==================
*/
void Host_ShutdownServer(bool crash)
{
    int i;
    int count;
    sizebuf_t buf;
    byte message[4];
    double start;

    if(!sv.active)
    {
        return;
    }

    sv.active = false;

    // stop all client sounds immediately
    if(cls.state == ca_connected)
    {
        CL_Disconnect();
    }

    // flush any pending messages - like the score!!!
    start = Sys_DoubleTime();
    do
    {
        count = 0;
        for(i = 0, host_client = svs.clients; i < svs.maxclients;
            i++, host_client++)
        {
            if(host_client->active && host_client->message.cursize &&
                host_client->netconnection /* QSS */)
            {
                if(NET_CanSendMessage(host_client->netconnection))
                {
                    NET_SendMessage(
                        host_client->netconnection, &host_client->message);
                    SZ_Clear(&host_client->message);
                }
                else
                {
                    NET_GetMessage(host_client->netconnection);
                    count++;
                }
            }
        }
        if((Sys_DoubleTime() - start) > 3.0)
        {
            break;
        }
    } while(count);

    // make sure all the clients know we're disconnecting
    buf.data = message;
    buf.maxsize = 4;
    buf.cursize = 0;
    MSG_WriteByte(&buf, svc_disconnect);
    count = NET_SendToAll(&buf, 5.0);
    if(count)
    {
        Con_Printf("Host_ShutdownServer: NET_SendToAll failed for %u clients\n",
            count);
    }

    // QSS
    PR_SwitchQCVM(&sv.qcvm);

    {
        for(i = 0, host_client = svs.clients; i < svs.maxclients;
            i++, host_client++)
        {
            if(host_client->active)
            {
                SV_DropClient(crash);
            }
        }

        // QSS
        qcvm->worldmodel = nullptr;
    }

    PR_SwitchQCVM(nullptr);

    //
    // clear structures
    //
    //	memset (&sv, 0, sizeof(sv)); // ServerSpawn already do this by
    // Host_ClearMemory
    memset(svs.clients, 0, svs.maxclientslimit * sizeof(client_t));
}


/*
================
Host_ClearMemory

This clears all the memory used by both the client and server, but does
not reinitialize anything.
================
*/
void Host_ClearMemory()
{
    Con_DPrintf("Clearing memory\n");
    D_FlushCaches();
    Mod_ClearAll();
    /* host_hunklevel MUST be set at this point */
    Hunk_FreeToLowMark(host_hunklevel);
    cls.signon = 0;

    PR_ClearProgs(&sv.qcvm);
    PR_ClearProgs(&cl.qcvm);

    free(cl.static_entities);
    free(sv.static_entities); // spike -- this is dynamic too, now
    free(sv.ambientsounds);

    sv = server_t{};
    cl = client_state_t{};
}


//==============================================================================
//
// Host Frame
//
//==============================================================================

/*
===================
Host_FilterTime

Returns false if the time is too short to run a frame
===================
*/
bool Host_FilterTime(float time)
{
    float maxfps; // johnfitz

    realtime += time;

    // johnfitz -- max fps cvar
    maxfps = CLAMP(10.0, host_maxfps.value, 1000.0);
    if(/* QSS */ host_maxfps.value && !cls.timedemo &&
        realtime - oldrealtime < 1.0 / maxfps && !vr_enabled.value)
    {
        return false; // framerate is too high
    }
    // johnfitz

    host_frametime = realtime - oldrealtime;
    oldrealtime = realtime;

    // johnfitz -- host_timescale is more intuitive than host_framerate
    if(host_timescale.value > 0)
    {
        host_frametime *= host_timescale.value;
        // johnfitz
    }
    else if(host_framerate.value > 0)
    {
        host_frametime = host_framerate.value;
    }
    else if(host_maxfps.value) // QSS
    {
        // don't allow really long or short frames
        host_frametime =
            CLAMP(0.0001, host_frametime, 0.1); // johnfitz -- use CLAMP
    }

    return true;
}

/*
===================
Host_GetConsoleCommands

Add them exactly as if they had been typed at the console
===================
*/
void Host_GetConsoleCommands()
{
    const char* cmd;

    if(!isDedicated)
    {
        return; // no stdin necessary in graphical mode
    }

    while(true)
    {
        cmd = Sys_ConsoleInput();
        if(!cmd)
        {
            break;
        }
        Cbuf_AddText(cmd);
    }
}

/*
==================
Host_ServerFrame
==================
*/
void Host_ServerFrame()
{
    int i;
    int active;   // johnfitz
    edict_t* ent; // johnfitz

    // run the world state
    pr_global_struct->frametime = host_frametime;

    // set the time and clear the general datagram
    SV_ClearDatagram();

    // check for new clients
    SV_CheckForNewClients();

    // read client messages
    SV_RunClients();

    // move things around and think
    // always pause in single player if in console or menus
    if(!sv.paused && (svs.maxclients > 1 || key_dest == key_game))
    {
        SV_Physics();
    }

    // johnfitz -- devstats
    if(cls.signon == SIGNONS)
    {

        // QSS
        for(i = 0, active = 0; i < qcvm->num_edicts; i++) // QSS
        {
            ent = EDICT_NUM(i);
            if(!ent->free)
            {
                active++;
            }
        }
        if(active > 600 && dev_peakstats.edicts <= 600)
        {
            Con_DWarning(
                "%i edicts exceeds standard limit of 600 (max = %d).\n", active,
                qcvm->max_edicts /* QSS */
            );
        }
        dev_stats.edicts = active;
        dev_peakstats.edicts = q_max(active, dev_peakstats.edicts);
    }
    // johnfitz

    // send all messages to the clients
    SV_SendClientMessages();
}

// QSS
// used for cl.qcvm.GetModel (so ssqc+csqc can share builtins)
qmodel_t* CL_ModelForIndex(int index)
{
    if(index < 0 || index >= MAX_MODELS)
    {
        return nullptr;
    }

    return cl.model_precache[index];
}

/*
==================
Host_Frame

Runs all active servers
==================
*/
void _Host_Frame(double time) // QSS
{
    static double accumtime = 0; // QSS
    static double time1 = 0;
    static double time2 = 0;
    static double time3 = 0;

    {
        host_abortserver_setjmp_done = true;
        if(setjmp(host_abortserver))
        {
            return; // something bad happened, or the server disconnected
        }
    }

    // keep the random time dependent
    rand();

    // decide the simulation time

    // QSS
    accumtime += host_netinterval ? CLAMP(0, time, 0.2)
                                  : 0; // for renderer/server isolation

    if(!Host_FilterTime(time))
    {
        return; // don't run too fast, or packets will flood out
    }

    // get new key events
    Key_UpdateForDest();
    IN_UpdateInputMode();
    Sys_SendKeyEvents();

    // allow mice or other external controllers to add commands
    IN_Commands();

    // QSS
    // check the stdin for commands (dedicated servers)
    Host_GetConsoleCommands();

    // process console commands
    Cbuf_Execute();

    NET_Poll();

    // QSS
    if(cl.sendprespawn)
    {
        if(CL_CheckDownloads())
        {
            PR_ClearProgs(&cl.qcvm);
            if(pr_checkextension.value && !cl_nocsqc.value)
            { // only try to use csqc if qc extensions are enabled.
                PR_SwitchQCVM(&cl.qcvm);
                // try csprogs.dat first, then fall back on progs.dat in case
                // someone tried merging the two. we only care about it if it
                // actually contains a CSQC_DrawHud, otherwise its either just a
                // (misnamed) ssqc progs or a full csqc progs that would just
                // crash us on 3d stuff.
                if((PR_LoadProgs("csprogs.dat", false, pr_csqcbuiltins,
                        pr_csqcnumbuiltins) &&
                       qcvm->extfuncs.CSQC_DrawHud) ||
                    (PR_LoadProgs("progs.dat", false, pr_csqcbuiltins,
                         pr_csqcnumbuiltins) &&
                        qcvm->extfuncs.CSQC_DrawHud))
                {
                    qcvm->max_edicts =
                        CLAMP(MIN_EDICTS, (int)max_edicts.value, MAX_EDICTS);
                    qcvm->edicts =
                        (edict_t*)malloc(qcvm->max_edicts * qcvm->edict_size);
                    qcvm->num_edicts = qcvm->reserved_edicts = 1;
                    memset(
                        qcvm->edicts, 0, qcvm->num_edicts * qcvm->edict_size);

                    // set a few globals, if they exist
                    if(qcvm->extglobals.maxclients)
                    {
                        *qcvm->extglobals.maxclients = cl.maxclients;
                    }
                    pr_global_struct->time = cl.time;
                    pr_global_struct->mapname = PR_SetEngineString(cl.mapname);
                    pr_global_struct->total_monsters =
                        cl.statsf[STAT_TOTALMONSTERS];
                    pr_global_struct->total_secrets =
                        cl.statsf[STAT_TOTALSECRETS];
                    pr_global_struct->deathmatch = cl.gametype;
                    pr_global_struct->coop =
                        (cl.gametype == GAME_COOP) && cl.maxclients != 1;
                    if(qcvm->extglobals.player_localnum)
                    {
                        *qcvm->extglobals.player_localnum =
                            cl.viewentity - 1; // this is a guess, but is
                    }
                    // important for scoreboards.

                    // set a few worldspawn fields too
                    qcvm->edicts->v.message = PR_SetEngineString(cl.levelname);

                    // and call the init function... if it exists.
                    qcvm->worldmodel = cl.worldmodel;
                    SV_ClearWorld();
                    if(qcvm->extfuncs.CSQC_Init)
                    {
                        G_FLOAT(OFS_PARM0) = 0;
                        G_INT(OFS_PARM1) =
                            PR_SetEngineString("QuakeSpasm-Spiked");
                        G_FLOAT(OFS_PARM2) = QUAKESPASM_VERSION;
                        PR_ExecuteProgram(qcvm->extfuncs.CSQC_Init);
                    }
                }
                else
                {
                    PR_ClearProgs(qcvm);
                }
                PR_SwitchQCVM(nullptr);
            }

            cl.sendprespawn = false;
            MSG_WriteByte(&cls.message, clc_stringcmd);
            MSG_WriteString(&cls.message, "prespawn");
            vid.recalc_refdef = true;
        }
        else
        {
            if(!cls.message.cursize)
            {
                MSG_WriteByte(&cls.message, clc_nop);
            }
        }
    }

    CL_AccumulateCmd();

    // Run the server+networking (client->server->client), at a different rate
    // from everyt
    if(accumtime >= host_netinterval)
    {
        float realframetime = host_frametime;
        if(host_netinterval)
        {
            host_frametime = q_max(accumtime, host_netinterval);
            accumtime -= host_frametime;

            if(host_timescale.value > 0)
            {
                host_frametime *= host_timescale.value;
            }
            else if(host_framerate.value)
            {
                host_frametime = host_framerate.value;
            }
        }
        else
        {
            accumtime -= host_netinterval;
        }

        CL_SendCmd();

        if(sv.active)
        {
            PR_SwitchQCVM(&sv.qcvm);
            Host_ServerFrame();
            PR_SwitchQCVM(nullptr);
        }

        host_frametime = realframetime;
        Cbuf_Waited();
    }

    // fetch results from server
    if(cls.state == ca_connected)
    {
        CL_ReadFromServer();

        // VR: Autosave.
        if(!deathmatch.value)
        {
            quake::saveutil::doAutomaticAutosave();
        }
    }

    // update video
    if(host_speeds.value)
    {
        time1 = Sys_DoubleTime();
    }

    SCR_UpdateScreen();

    CL_RunParticles(); // johnfitz -- seperated from rendering

    if(host_speeds.value)
    {
        time2 = Sys_DoubleTime();
    }

    // update audio
    BGM_Update(); // adds music raw samples and/or advances midi driver
    if(cls.signon == SIGNONS)
    {
        S_Update(r_origin, vpn, vright, vup);
        CL_DecayLights();
    }
    else
    {
        S_Update(vec3_zero, vec3_zero, vec3_zero, vec3_zero);
    }

    CDAudio_Update();

    if(host_speeds.value)
    {
        int pass1 = (time1 - time3) * 1000;
        time3 = Sys_DoubleTime();
        int pass2 = (time2 - time1) * 1000;
        int pass3 = (time3 - time2) * 1000;
        Con_Printf("%3i tot %3i server %3i gfx %3i snd\n",
            pass1 + pass2 + pass3, pass1, pass2, pass3);
    }

    host_framecount++;
}

void Host_Frame(double time) // QSS
{
    double time1;
    double time2;
    static double timetotal;
    static int timecount;
    int i;
    int c;
    int m;

    if(!serverprofile.value)
    {
        _Host_Frame(time);
        return;
    }

    time1 = Sys_DoubleTime();
    _Host_Frame(time);
    time2 = Sys_DoubleTime();

    timetotal += time2 - time1;
    timecount++;

    if(timecount < 1000)
    {
        return;
    }

    m = timetotal * 1000 / timecount;
    timecount = 0;
    timetotal = 0;
    c = 0;
    for(i = 0; i < svs.maxclients; i++)
    {
        if(svs.clients[i].active)
        {
            c++;
        }
    }

    Con_Printf("serverprofile: %2i clients %2i msec\n", c, m);
}

/*
====================
Host_Init
====================
*/
void Host_Init()
{
    if(standard_quake)
    {
        minimum_memory = MINIMUM_MEMORY;
    }
    else
    {
        minimum_memory = MINIMUM_MEMORY_LEVELPAK;
    }

    if(COM_CheckParm("-minmemory"))
    {
        host_parms->memsize = minimum_memory;
    }

    if(host_parms->memsize < minimum_memory)
    {
        Sys_Error("Only %4.1f megs of memory available, can't execute game",
            host_parms->memsize / (float)0x100000);
    }

    com_argc = host_parms->argc;
    com_argv = host_parms->argv;

    Memory_Init(host_parms->membase, host_parms->memsize);
    Cbuf_Init();
    Cmd_Init();
    LOG_Init(host_parms);
    Cvar_Init(); // johnfitz
    VR_InitCvars();
    COM_Init();
    COM_InitFilesystem();
    Host_InitLocal();
    W_LoadWadFile(); // johnfitz -- filename is now hard-coded for honesty
    if(cls.state != ca_dedicated)
    {
        Key_Init();
        Con_Init();
    }
    PR_Init();
    Mod_Init();
    NET_Init();
    SV_Init();

    Con_Printf("Exe: " __TIME__ " " __DATE__ "\n");
    Con_Printf("%4.1f megabyte heap\n", host_parms->memsize / (1024 * 1024.0));

    if(cls.state != ca_dedicated)
    {
        host_colormap = (byte*)COM_LoadHunkFile("gfx/colormap.lmp", nullptr);
        if(!host_colormap)
        {
            Sys_Error("Couldn't load gfx/colormap.lmp");
        }

        V_Init();
        Chase_Init();
        M_Init();
        ExtraMaps_Init(); // johnfitz
        Modlist_Init();   // johnfitz
        DemoList_Init();  // ericw
        VID_Init();
        IN_Init();
        TexMgr_Init(); // johnfitz
        Draw_Init();
        SCR_Init();
        R_Init();
        S_Init();
        CDAudio_Init();
        BGM_Init();
        Sbar_Init();
        CL_Init();
    }

    (void)Hunk_AllocName(0, "-HOST_HUNKLEVEL-");
    host_hunklevel = Hunk_LowMark();

    host_initialized = true;
    Con_Printf("\n========= Quake Initialized =========\n\n");

    // QSS
    // spike -- create these aliases, because they're useful.
    Cbuf_AddText("alias startmap_sp \"map start\"\n");
    Cbuf_AddText("alias startmap_dm \"map start\"\n");

    if(cls.state != ca_dedicated)
    {
        // QSS
        Cbuf_AddText("cl_warncmd 0\n");

        // VR: This is what reads 'config.cfg'.
        Cbuf_InsertText("exec quake.rc\n");
        Cbuf_AddText("cl_warncmd 1\n");
        Cbuf_Execute();

        if(vr_enabled.value == 1)
        {
            VR_ModAllModels();
        }

        // johnfitz -- in case the vid mode was locked during vid_init, we
        // can unlock it now. note: two leading newlines because the command
        // buffer swallows one of them.
        Cbuf_AddText("\n\nvid_unlock\n");
    }

    if(cls.state == ca_dedicated)
    {
        // QSS
        Cbuf_AddText("cl_warncmd 0\n");
        Cbuf_AddText(
            "exec default.cfg\n"); // spike -- someone decided that quake.rc
                                   // shouldn't be execed on dedicated servers,
                                   // but that means you'll get bad defaults
        Cbuf_AddText("cl_warncmd 1\n");
        Cbuf_AddText("exec server.cfg\n"); // spike -- for people who want
                                           // things explicit.

        Cbuf_AddText("exec autoexec.cfg\n");
        Cbuf_AddText("stuffcmds");
        Cbuf_Execute();

        if(!sv.active)
        {
            // QSS
            Cbuf_AddText("startmap_dm\n");
        }
    }
}


/*
===============
Host_Shutdown

FIXME: this is a callback from Sys_Quit and Sys_Error.  It would be better
to run quit through here before the final handoff to the sys code.
===============
*/
void Host_Shutdown()
{
    static bool isdown = false;

    if(isdown)
    {
        printf("recursive shutdown\n");
        return;
    }
    isdown = true;

    // keep Con_Printf from trying to update the screen
    scr_disabled_for_loading = true;

    Host_WriteConfiguration();

    NET_Shutdown();

    if(cls.state != ca_dedicated)
    {
        if(con_initialized)
        {
            History_Shutdown();
        }
        BGM_Shutdown();
        CDAudio_Shutdown();
        S_Shutdown();
        IN_Shutdown();
        VID_VR_Shutdown();
        VID_Shutdown();
    }

    LOG_Close();
}
