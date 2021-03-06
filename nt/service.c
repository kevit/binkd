/*
 *  service.c -- Windows NT services support for binkd
 *
 *  service.c is a part of binkd project
 *
 *  Copyright (C) 2000 Dima Afanasiev, da@4u.net (Fido 2:5020/463)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version. See COPYING.
 */
/* About Windows NT services see http://www.microsoft.com/msj/0298/service.aspx
    Jeffrey Richter. 
    "Manipulate Windows NT Services by Writing a Service Control Program"
 */

#include <stdio.h>
#include <process.h>
#include <io.h>
#include <direct.h>
#include <string.h>
#include <malloc.h>
#include "../sys.h"
#include "../iphdr.h"
#include "../readcfg.h"
#include "../tools.h"
#include "../common.h"
#include "service.h"
#include "w32tools.h"
#include "tray.h"

/* service_status() and query_status() returns this to indicate error */
#define SERVICE_STATUS_ERROR -2

/* Registry values types */
#define args_REG_type REG_BINARY /* REG_MULTI_SZ */
#define path_REG_type REG_SZ

/* ChangeServiceConfig2() prototype:
 */
typedef BOOL (WINAPI *CSD_T)(SC_HANDLE, DWORD, LPCVOID);

extern enum serviceflags service_flag;
extern char *configpath;

static const char libname[]="ADVAPI32";
char *srvname = "binkd-service";
static const char *description = "BinkD: Fidonet TCP/IP mailer uses binkp protocol";
static const char dependencies[] = "Tcpip\0Afd\0"; /* Afd is the winsock handler */
static char reg_path_prefix[]="SYSTEM\\CurrentControlSet\\Services\\";
static char reg_path_suffix[]="\\Parameters";
static SERVICE_STATUS_HANDLE sshan;
static SERVICE_STATUS sstat;
static int res_checkservice=0;
static DWORD dwErr=0;
static char **serv_argv=NULL;
extern int checkcfg_flag;
MUTEXSEM exitsem=NULL;
static DWORD srvtype = SERVICE_WIN32_OWN_PROCESS;
static SC_HANDLE sman=NULL, shan=NULL;

static void try_open_SCM(void);
static void try_open_service(void);


BOOL ReportStatusToSCMgr(DWORD dwCurrentState,
                         DWORD dwWin32ExitCode,
                         DWORD dwWaitHint)
{
  static DWORD dwCheckPoint = 1;
  BOOL fResult = TRUE;

  Log(12,"ReportStatusToSCMgr(%lu, %lu, %lu)",dwCurrentState,dwWin32ExitCode,dwWaitHint);

  if (dwCurrentState == SERVICE_START_PENDING)
    sstat.dwControlsAccepted = 0;
  else
    sstat.dwControlsAccepted = SERVICE_ACCEPT_STOP;
  sstat.dwCurrentState = dwCurrentState;
  sstat.dwWin32ExitCode = dwWin32ExitCode;
  sstat.dwWaitHint = dwWaitHint;

  if ( ( dwCurrentState == SERVICE_RUNNING ) ||
       ( dwCurrentState == SERVICE_STOPPED ) )
    sstat.dwCheckPoint = 0;
  else
    sstat.dwCheckPoint = dwCheckPoint++;

  fResult = SetServiceStatus( sshan, &sstat);
  return fResult;
}

/* wrapper to call exitfunc() in separate thread
 * serviceexitproc() must be used in parameter of _beginthread()
 */
void __cdecl serviceexitproc(void *arg)
{
  Log(10,"serviceexitproc()");
  exitfunc();
/*  ReportStatusToSCMgr(SERVICE_STOPPED, NO_ERROR, 0); *//* exitfunc() kill thread and this line don't executed */
}

/* Service control handler. Called by system's SCM
 */
static void WINAPI ServiceCtrl(DWORD dwCtrlCode)
{
  switch(dwCtrlCode)
  {
  case SERVICE_CONTROL_STOP:
    ReportStatusToSCMgr(SERVICE_STOP_PENDING, NO_ERROR, 0);
    Log(1, "Interrupted by service stop");
/*    SigHandler(CTRL_SERVICESTOP_EVENT); */ /* Only report to log "Interrupted by service stop" */
/*    exit(0); */ /* Produce SCM error "109" */
    if( BEGINTHREAD(&serviceexitproc,0,NULL) != -1 )
      return;
    else
      exit(0);   /* may be need print message to log about error? */
  case SERVICE_CONTROL_INTERROGATE:
  default:
    break;
  }
  ReportStatusToSCMgr(sstat.dwCurrentState, NO_ERROR, 0);
}

/* Start service-specific cleanup procedure.
 * Must be set in last call of atexit().
 * (Report to SCM about service stop pending)
 */
void atServiceExitBegins(void)
{
  Log(10,"atServiceExitBegins()");
  if(IsNT() && isService())
    ReportStatusToSCMgr(SERVICE_STOP_PENDING, NO_ERROR, 0);
}

/* Service-specific cleanup procedure. Not an thread-safe!
 * Must be set in first call of atexit().
 */
void atServiceExitEnds(void)
{
  char *sp;
  Log(10,"atServiceExitEnds()");

  if(!IsNT() || !isService())
    return;

  if(serv_argv)
  {
    sp=serv_argv[0];
    free(serv_argv);
    if(sp)free(sp);
    serv_argv=NULL;
  }
  CleanSem(&exitsem);
  ReportStatusToSCMgr(SERVICE_STOPPED, NO_ERROR, 0);
}

/* Retrieve value from registry. Return length of data.
 * hk      - (in) registry key handle (created using RegOpenKey())
 * name    - (in) name of the value to query
 * reqtype - (in) type of the value to query
 * data    - (out) pointer to array of bytes (function stores malloc'ed array)
 */
static int get_string_registry_value(HKEY hk, char *name, DWORD reqtype, char **data)
{ DWORD type, size=0;
  LPBYTE pdata=NULL;
  LONG rc;

  if( !hk )
  {
    AlertWin("get_string_registry_value(): Parameter 'hk' is NULL");
    return 0;
  }
  if( !data )
  {
    AlertWin("get_string_registry_value(): Parameter 'data' is NULL");
    return 0;
  }
  *data=NULL;
  rc = RegQueryValueEx(hk, name, NULL, &type, pdata, &size);
  if( type!=reqtype )
  { char ttt[512];
    snprintf(ttt,sizeof(ttt),"Incompatible type of registry value '%s'", (name&&name[0])?name:"Default" );
    AlertWin(ttt);
    return 0;
  }
  if( rc!=ERROR_SUCCESS && rc!=ERROR_MORE_DATA )
  {
    AlertWin(w32err(rc));
    return 0;
  }
  if( !(pdata=(LPBYTE)malloc(size)) )
  {
    AlertWin( "Low memory");
    return 0;
  }
  if( RegQueryValueEx(hk, name, NULL, &type, pdata, &size)!=ERROR_SUCCESS )
  {
    free(pdata);
    return 0;
  }
  *data = (char*)pdata;
  return size;
}

/* Retrieve the "service display name" from SCM
   Return malloc'ed string or NULL
 */
static char* NTServiceDisplayName(char*ServiceName)
{ char DisplayName[256]; /* MSDN spoke: "The maximum string length is 256 characters" */
  DWORD Len=sizeof(DisplayName);
  char* ret=NULL;

  if(!ServiceName) return NULL;
  if(!sman) try_open_SCM();
  if(sman)
  { 
    if( GetServiceDisplayName( sman, ServiceName, DisplayName, &Len) )
      ret = strdup(DisplayName);
    else
    { if( GetLastError()==ERROR_MORE_DATA && Len )
      { char* DName=malloc(Len);
        if( DName && GetServiceDisplayName( sman, ServiceName, DName, &Len) )
          ret = DName;
      }
    }
  }

  return ret?ret:strdup(ServiceName);
}


static void ServiceStart()
{
  HKEY hk=NULL;
  DWORD sw=MAXPATHLEN+1;
  int i, argc;
  char *sp=(char*)malloc(sw);

  InitSem (&exitsem); /* See exitproc.c */

  if(!sp) AlertWin("Low memory!");
  strnzcpy(sp, reg_path_prefix,sw);
  strnzcat(sp, srvname,sw);
  strnzcat(sp, reg_path_suffix,sw);

  atexit(atServiceExitEnds);

  if( RegOpenKey(HKEY_LOCAL_MACHINE, sp, &hk)==ERROR_SUCCESS)
  {
    { /* Read and use key "path" */
      char *path = NULL;
      if( 0==get_string_registry_value(hk, "path", path_REG_type, &path) )
      { char ttt[MAXPATHLEN+70];
        snprintf(ttt,sizeof(ttt),"Can't read value of \"path\" from registry key HKLM\\%s",sp);
        AlertWin(ttt);
        free(sp);
        goto ServiceStart_error;
      }
      SetCurrentDirectory(path);
      free(path);
    }
    { /* Read and parse key "args" */
      char *args=NULL;
      int sz;
      sz = get_string_registry_value(hk, "args", args_REG_type, &args);
      if(!sz)
      { char ttt[MAXPATHLEN+70];
        snprintf(ttt,sizeof(ttt),"Can't read value of \"args\" from registry key HKLM\\%s",sp);
        AlertWin(ttt);
        free(sp);
        goto ServiceStart_error;
      }
      for(i=argc=0;args[i];i+=strlen(args+i)+1) argc++;
      serv_argv=(char**)malloc(sizeof(char*)*(argc+1));
      for(i=argc=0;args[i];i+=strlen(args+i)+1) serv_argv[argc++]=args+i;
      serv_argv[argc]=NULL;
    }

    if (!ReportStatusToSCMgr(SERVICE_RUNNING, NO_ERROR, 0))
    {
      dwErr=GetLastError();
      goto ServiceStart_error;
    }

    service_name = NTServiceDisplayName(srvname);

    binkd_main(argc, serv_argv);
  }
  else
  {
    dwErr=GetLastError();
  }

ServiceStart_error:

  if(hk) RegCloseKey(hk);
}


static void WINAPI ServiceMain(DWORD argc,LPSTR* args)
{

  if(argc && args && args[0]) srvname = strdup(args[0]); /* save service name */
  service_flag = w32_run_as_service;

  sshan=RegisterServiceCtrlHandler(srvname, ServiceCtrl);
  if(sshan)
  {
    sstat.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    sstat.dwServiceSpecificExitCode = 0;
    if (ReportStatusToSCMgr(SERVICE_START_PENDING, NO_ERROR, 3000))
    {
      ServiceStart();
    }
  }
  exit(0);
}


static void cleanup_service(void)
{
  if(shan)
  {
    CloseServiceHandle(shan);
    shan = NULL;
  }
  if(sman)
  {
    CloseServiceHandle(sman);
    sman = NULL;
  }
}

static void try_open_SCM(void)
{
  if(!sman)
  { DWORD sc_manager_desired_access=0;
    if( isService() )
      sc_manager_desired_access=SC_MANAGER_CONNECT;
    else switch(service_flag){
      case w32_installservice:
        sc_manager_desired_access=SC_MANAGER_CREATE_SERVICE;
           break;
      case w32_uninstallservice:
      case w32_startservice:
      case w32_stopservice:
      case w32_restartservice:
      case w32_queryservice:
      case w32_run_as_service:
        sc_manager_desired_access=SC_MANAGER_CONNECT;
           break;
      default: return;
    }
    sman = OpenSCManager(NULL, NULL, sc_manager_desired_access);
    if(!sman)
    { int err = GetLastError();
      if(res_checkservice)
        Log(isService()?1:-1, "OpenSCManager failed: %s",w32err(err));
      else if(err==ERROR_ACCESS_DENIED)
      {
        Log(isService()?1:-1, "%s access to NT service controls is denied.", isService()? "Any" : "R/W");
      }
    }
  }
}

static void try_open_service(void)
{
  try_open_SCM();
  if( sman && !shan ){
    DWORD service_desired_access;
    switch(service_flag){
      case w32_installservice:
        service_desired_access=SERVICE_ALL_ACCESS;
           break;
      case w32_uninstallservice:
        service_desired_access=DELETE|SERVICE_STOP|SERVICE_QUERY_STATUS;
           break;
      case w32_startservice:
        service_desired_access=SERVICE_START|SERVICE_QUERY_STATUS;
           break;
      case w32_stopservice:
        service_desired_access=SERVICE_STOP|SERVICE_QUERY_STATUS;
           break;
      case w32_restartservice:
        service_desired_access=SERVICE_STOP|SERVICE_START|SERVICE_QUERY_STATUS;
           break;
      case w32_queryservice:
        service_desired_access=SERVICE_QUERY_STATUS;
           break;
      case w32_run_as_service:
        service_desired_access=SERVICE_INTERROGATE; /* SERVICE_CONTROL_INTERROGATE */
           break;
      default: return;
    }
    shan=OpenService(sman, srvname, service_desired_access);
  }
}


/* Return current service status or -2 (SERVICE_STATUS_ERROR)
 */
static DWORD service_status(void)
{
  if( !shan ) try_open_service();
  if( !shan ) return SERVICE_STATUS_ERROR;
  /* ControlService is more accurate status checks what QueryServiceStatus() */
  if( !ControlService(shan,SERVICE_CONTROL_INTERROGATE,&sstat) )
    switch(GetLastError())
    {
      case NO_ERROR:
      case ERROR_INVALID_SERVICE_CONTROL:
      case ERROR_SERVICE_CANNOT_ACCEPT_CTRL:
      case ERROR_SERVICE_NOT_ACTIVE:
        break;
      default:
        if( !QueryServiceStatus(shan,&sstat) ) return SERVICE_STATUS_ERROR;
    }
  return sstat.dwCurrentState;
}


/* Return 0 if service status is equal with parameter.
 */
static int query_service(DWORD servicestatus)
{
  return !(service_status() == servicestatus);
}


static int store_data_into_registry(char **argv,char **envp)
{ int len, rc=0;
  char sp[MAXPATHLEN+1];
  char *asp=NULL;
  HKEY hk=0;

  strnzcpy(sp, reg_path_prefix,sizeof(sp));
  strnzcat(sp, srvname,sizeof(sp));
  if( RegOpenKey(HKEY_LOCAL_MACHINE, sp, &hk)==ERROR_SUCCESS )
  {
    RegCloseKey(hk);
    hk = NULL;
  }
  else
  {
    Log(1, "Unable to open registry key %s!", sp);
    return -1;
  }
  strnzcat(sp, reg_path_suffix,sizeof(sp));

  /* build arguments list for service */
  len = build_service_arguments((char**)(&asp), argv, 0);

  if(  (  (RegOpenKey(HKEY_LOCAL_MACHINE, sp, &hk)==ERROR_SUCCESS)
        ||(RegCreateKey(HKEY_LOCAL_MACHINE, sp, &hk)==ERROR_SUCCESS)
       )
     &&(RegSetValueEx(hk, "args", 0, args_REG_type, (BYTE*)asp, len)==ERROR_SUCCESS)
     &&(GetCurrentDirectory(sizeof(sp), sp)>0)
     &&(RegSetValueEx(hk, "path", 0, path_REG_type, (BYTE*)sp, strlen(sp))==ERROR_SUCCESS)
    )
  {
      rc = 0;
  }
  else
  {
    rc = -1;
    Log(1, "Unable to store data in registry for service '%s'", service_name);
    res_checkservice=(CHKSRV_CANT_INSTALL);
  }
  if(hk) RegCloseKey(hk);
  xfree(asp);

  return rc;
}

static int install_service(char **argv,char **envp)
{

  if(!sman) try_open_service();
  if(!sman) return -1;

  if(!shan)
  {
    char path[MAXPATHLEN+1];
    if(GetModuleFileName(NULL, path, MAXPATHLEN)<1)
    {
      Log(1, "Error in GetModuleFileName()=%s", w32err(GetLastError()) );
      CloseServiceHandle(sman);
      return -1;
    }
    shan=CreateService( sman,                 /* SCManager database */
                        srvname,              /* name of service */
                        service_name,         /* name to display */
                        SERVICE_ALL_ACCESS,   /* desired access */
                        srvtype,              /* service type */
                        SERVICE_AUTO_START,   /* start type */
                        SERVICE_ERROR_NORMAL, /* error control type */
                        path,                 /* service's binary */
                        NULL,                 /* no load ordering group */
                        NULL,                 /* no tag identifier */
                        dependencies,         /* dependencies */
                        NULL,                 /* user account */
                        NULL );               /* account password */

    if(!shan)
    {
      Log(1, "Error in CreateService()=%s", w32err(GetLastError()) );
      return -1;
    }
    else
    {
      CSD_T ChangeServiceDescription;
      HANDLE hwin2000scm;

      hwin2000scm = GetModuleHandle(libname);
      if (hwin2000scm) {
        ChangeServiceDescription = (CSD_T) GetProcAddress(hwin2000scm,
                                                  "ChangeServiceConfig2A");
        if (ChangeServiceDescription)
        {
          ChangeServiceDescription( shan,
                                    1 /* SERVICE_CONFIG_DESCRIPTION */,
                                    &description );
        }
      }
    }
    return store_data_into_registry(argv,envp);
  }
  Log(-1, "Service \"%s\" installed successfully.", service_name);
  return 0;
}

static int wait_service_operation(DWORD pending_state, DWORD finish_state)
{
  int i;
  DWORD state;

  for( i=0; (i<30)&&((state=service_status())>0); i++ )
  {
    if((state==pending_state)||(state!=finish_state)||(i<3))
    {
      printf(".");
      Sleep(i<3?100:300);
    }
    else break;
  }
  putchar('\n');

  return (sstat.dwCurrentState != finish_state);
}

static int start_service(void)
{

  if(!sman) try_open_service();
  if(!sman) return -1;
  if(!shan)
  { DWORD err=GetLastError();
    switch( err ){
    case ERROR_SERVICE_DOES_NOT_EXIST:
      Log(-1, "Service \"%s\" is not installed, can't start service.", service_name);
      break;
    case ERROR_ACCESS_DENIED:
      Log(1, "Access to starting service \"%s\" is denied...", service_name);
      break;
    default:
      Log(1, "Error at starting service \"%s\": %s", err, service_name, w32err(err) );
    }
    return -1;
  }
  if( query_service(SERVICE_RUNNING)==0 )
  {
    Log(-1, "Service \"%s\" already started.", service_name);
    return 0;
  }

  if( /*query_service(SERVICE_START_PENDING) &&*/ !StartService(shan, 0, NULL) )
  {
    Log(1, "Error in StartService()=%s", w32err(GetLastError()) );
    return -1;
  }

  if( wait_service_operation(SERVICE_START_PENDING,SERVICE_RUNNING) )
  {
    Log(-1, "Unable to start service '%s'.", service_name);
    return -1;
  }

  Log(-1, "Service '%s' started successfully.", service_name);
  return 0;
}

static int stop_service(void)
{

  if(!sman) try_open_service();
  if(!sman) return -1;
  if(!shan)
  { DWORD err=GetLastError();
    switch( err ){
    case ERROR_SERVICE_DOES_NOT_EXIST:
      Log(-1, "Service \"%s\" is not installed...", service_name);
      break;
    case ERROR_ACCESS_DENIED:
      Log(1, "Access to control service \"%s\" is denied!", service_name);
      break;
    default:
      Log(1, "Error at stopping service \"%s\": %s", err, service_name, w32err(err) );
    }
    return -1;
  }

  if( query_service(SERVICE_STOPPED)==0 )
  {
    Log(-1, "Service \"%s\" is not running...", service_name);
    return 0;
  }

  if( ControlService(shan, SERVICE_CONTROL_STOP, &sstat) &&
      wait_service_operation(SERVICE_STOP_PENDING,SERVICE_STOPPED) )
  {
    Log(1, "Unable to service '%s'!", service_name);
    return -1;
  }

  Log(-1, "Service '%s' stopped successfully.", service_name);
  return 0;
}

static int uninstall_service(void)
{

  if(!sman) try_open_service();
  if(!sman) return -1;
  if(!shan)
  { DWORD err=GetLastError();
    switch( err ){
    case ERROR_SERVICE_DOES_NOT_EXIST:
      Log(-1, "Service \"%s\" is not installed...", service_name);
      break;
    case ERROR_ACCESS_DENIED:
      Log(1, "Access to uninstalling service \"%s\" is denied!", service_name);
      break;
    default:
      Log(1, "Error at uninstalling service \"%s\": %s", err, service_name, w32err(err) );
    }
    return -1;
  }

  if( !DeleteService(shan) )
  {
    Log(1, "Error in DeleteService()=%s", w32err(GetLastError()) );
    return -1;
  }

  Log(-1, "Service '%s' uninstalled successfully.", service_name);
  return 0;
}



int service(int argc, char **argv, char **envp)
{
  int j, rc=0;

  if(service_flag==w32_noservice) return 0;
  else{
    if( !IsNT() )
    {
      Log(0,"Can't operate witn Windows NT services: incompatible OS type");
      return 1;
    }
  }

  if(service_name) srvname = get_service_name(service_name);  /* Use service name from command line if specified */
  else service_name = srvname;

  if (tray_flag)
  {
     srvtype |= SERVICE_INTERACTIVE_PROCESS;
  }

  j=checkservice();

  if(j==CHKSRV_ERROR){
      Log(0, "Can't operate witn Windows NT services: %s", w32err(GetLastError()));
  }
  if(j==CHKSRV_CANT_INSTALL && !isService()){
      Log(0, "Can't operate witn Windows NT services...");
  }

  switch(service_flag){

  case w32_installservice:
    if (j==CHKSRV_INSTALLED){
      Log(-1, "Service '%s' already installed...", service_name);
      exit(0);
    }else{

      rc = install_service(argv,envp);

      if( !rc && !(rc=start_service()) )
        exit(0);
      rc = -1;
    }
    break;

  case w32_uninstallservice:
    if (j==CHKSRV_NOT_INSTALLED)
      Log(-1, "Service '%s' already uninstalled...", service_name);
    else{
      rc = stop_service() + uninstall_service();
    }
    cleanup_service();
    exit(0);
    break;

  case w32_startservice:
    start_service();
    cleanup_service();
    exit(0);
    break;

  case w32_stopservice:
    stop_service();
    cleanup_service();
    exit(0);
    break;

  case w32_restartservice:
    if( !stop_service() ) start_service();
    cleanup_service();
    exit(0);
    break;

  case w32_queryservice:
    if( j==CHKSRV_NOT_INSTALLED )
      Log(-1, "Service '%s' is not installed.", service_name);
    else if( j==CHKSRV_INSTALLED )
    { char *statustext="";
      switch (service_status())
      {
        case SERVICE_STATUS_ERROR: statustext="but status is unknown (query status error)"; break;
        case SERVICE_NO_CHANGE: statustext="and status is 'no change'"; break;
        case SERVICE_STOPPED: statustext="but is stopped"; break;
        case SERVICE_START_PENDING: statustext="and start in progress"; break;
        case SERVICE_STOP_PENDING: statustext="and stop in progress"; break;
        case SERVICE_RUNNING: statustext="and is running"; break;
        case SERVICE_CONTINUE_PENDING: statustext="and continue in progress from pause"; break;
        case SERVICE_PAUSE_PENDING: statustext="and pause in progress"; break;
        case SERVICE_PAUSED: statustext="and is paused"; break;
        default: statustext="but status is unknown"; break;
      }
      Log(-1, "Service '%s' is installed %s.", service_name, statustext);
    }

    cleanup_service();
    exit(0);
    break;

  default:
    break;
  }
  cleanup_service();
  return rc;
}

/* Return:
 * -1 : can't install service
 *  1 : Service not installed
 *  2 : Service installed
 */
int checkservice(void)
{
  if(res_checkservice) return res_checkservice;

  if(!IsNT()) return res_checkservice=(CHKSRV_CANT_INSTALL);

  try_open_service();
  if(!sman)
    return res_checkservice=CHKSRV_CANT_INSTALL;
  if(!shan)
  { DWORD err=GetLastError();
    switch( err )
    {
      case ERROR_SERVICE_DOES_NOT_EXIST:
        return res_checkservice=CHKSRV_NOT_INSTALLED;
      default:
        return res_checkservice=CHKSRV_ERROR;
    }
  }

  return res_checkservice=CHKSRV_INSTALLED;
}

/* Try connect to NT service controller
 * Return 1 if program running standalone or system error
 */
int tell_start_ntservice(void)
{
  SERVICE_TABLE_ENTRY dt[]= { {"", ServiceMain}, {NULL, NULL}};
  int res=0;

  if( !isService() )
    return 1;

  if(!StartServiceCtrlDispatcher(dt)){
    switch( GetLastError() ){           /* Can't start service */
    case ERROR_FAILED_SERVICE_CONTROLLER_CONNECT:  /*1063*/
       res=1;   /* Program running not an as service */
       break;
    case ERROR_SERVICE_ALREADY_RUNNING:
       Log(-1,"Error %u: Double call of StartServiceCtrlDispatcher()",ERROR_SERVICE_ALREADY_RUNNING);
       break;
    case ERROR_INVALID_DATA:
       Log(-1,"Error %u: The specified dispatch table contains entries that are not in the proper format.", ERROR_INVALID_DATA);
       break;
    default:
       Log(-1, "tell_start_ntservice(): %s", w32err(GetLastError()) );
    }
  }
  return res;
}

void do_tray_flag(void)
{
#ifdef HAVE_THREADS
  BEGINTHREAD(wndthread, 0, NULL);
#endif
}
