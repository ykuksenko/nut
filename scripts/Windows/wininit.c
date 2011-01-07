/* wininit.c - MS Windows service which replace the init script

   Copyright (C)
	2010	Frederic Bohe <fredericbohe@eaton.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/
#ifdef WIN32

#include <windows.h>
#include "common.h"
#include "config.h"
#include "winevent.h"

#define NUT_START	TRUE
#define NUT_STOP	FALSE

typedef struct conn_s {
	HANDLE		handle;
	OVERLAPPED	overlapped;
	char		buf[LARGEBUF];
	struct conn_s	*prev;
	struct conn_s	*next;
} conn_t;

static DWORD			upsd_pid = 0;
static DWORD			upsmon_pid = 0;
static HANDLE			pipe_connection_handle;
static OVERLAPPED		pipe_connection_overlapped;
static conn_t			*connhead = NULL;
static BOOL			service_flag = TRUE;
HANDLE				svc_stop = NULL;
static SERVICE_STATUS		SvcStatus;
static SERVICE_STATUS_HANDLE	SvcStatusHandle;

static void print_event(DWORD priority, const char * string)
{
	HANDLE EventSource;

	EventSource = RegisterEventSource(NULL, SVCNAME);

	if( NULL != EventSource ) {
		ReportEvent(    EventSource,	/* event log handle */
				priority,	/* event type */
				0,		/* event category */
				SVC_EVENT,	/* event identifier */
				NULL,		/* no security identifier*/
				1,		/* size of string array */
				0,		/* no binary data */
				&string,	/* array of string */
				NULL);		/* no binary data */

		DeregisterEventSource(EventSource);

	}

}

static void pipe_create()
{
	BOOL ret;

	if( pipe_connection_overlapped.hEvent != 0 ) {
		CloseHandle(pipe_connection_overlapped.hEvent);
	}
	memset(&pipe_connection_overlapped,0,sizeof(pipe_connection_overlapped));
	pipe_connection_handle = CreateNamedPipe(
			EVENTLOG_PIPE_NAME,	/* pipe name */
			PIPE_ACCESS_INBOUND |	/* to server only */
			FILE_FLAG_OVERLAPPED,	/* async IO */
			PIPE_TYPE_MESSAGE |
			PIPE_READMODE_MESSAGE |
			PIPE_WAIT,
			PIPE_UNLIMITED_INSTANCES, /* max. instances */
			LARGEBUF,		/* output buffer size */
			LARGEBUF,		/* input buffer size */
			0,			/* client time-out */
			NULL);	/* FIXME: default security attribute */

	if (pipe_connection_handle == INVALID_HANDLE_VALUE) {
		print_event(LOG_ERR,"Error creating named pipe");
		fatal_with_errno(EXIT_FAILURE, "Can't create a state socket (windows named pipe)");
	}

	/* Prepare an async wait on a connection on the pipe */
	pipe_connection_overlapped.hEvent = CreateEvent(NULL, /*Security*/
			FALSE, /* auto-reset*/
			FALSE, /* inital state = non signaled*/
			NULL /* no name*/);
	if(pipe_connection_overlapped.hEvent == NULL ) {
		print_event(LOG_ERR,"Error creating event");
		fatal_with_errno(EXIT_FAILURE, "Can't create event");
	}

	/* Wait for a connection */
	ret = ConnectNamedPipe(pipe_connection_handle,&pipe_connection_overlapped);
	if(ret == 0 && GetLastError() != ERROR_IO_PENDING ) {
		print_event(LOG_ERR,"ConnectNamedPipe error");
	}
}

static void pipe_connect()
{
	/* We have detected a connection on the opened pipe. So we start by saving its handle and create a new pipe for future connections */
	conn_t *conn;

	conn = xcalloc(1,sizeof(*conn));
	conn->handle = pipe_connection_handle;

	/* restart a new listening pipe */	
	pipe_create();

	/* A new pipe waiting for new client connection has been created. We could manage the current connection now */
	/* Start a read operation on the newly connected pipe so we could wait on the event associated to this IO */
	memset(&conn->overlapped,0,sizeof(conn->overlapped));
	memset(conn->buf,0,sizeof(conn->buf));
	conn->overlapped.hEvent = CreateEvent(NULL, /*Security*/
			FALSE, /* auto-reset*/
			FALSE, /* inital state = non signaled*/
			NULL /* no name*/);
	if(conn->overlapped.hEvent == NULL ) {
		print_event(LOG_ERR,"Can't create event for reading event log");
		return;
	}

	ReadFile (conn->handle,conn->buf,sizeof(conn->buf)-1,NULL,&(conn->overlapped)); /* -1 to be sure to have a trailling 0 */

	if (connhead) {
		conn->next = connhead;
		connhead->prev = conn;
	}

	connhead = conn;
}

static void pipe_disconnect(conn_t *conn)
{
	if( conn->overlapped.hEvent != INVALID_HANDLE_VALUE) {
		CloseHandle(conn->overlapped.hEvent);
		conn->overlapped.hEvent = INVALID_HANDLE_VALUE;
	}
	if ( DisconnectNamedPipe(conn->handle) == 0 ) {
		print_event(LOG_ERR,"DisconnectNamedPipe");
	}

	if (conn->prev) {
		conn->prev->next = conn->next;
	} else {
		connhead = conn->next;
	}

	if (conn->next) {
		conn->next->prev = conn->prev;
	} else {
		/* conntail = conn->prev; */
	}

	free(conn);
}

static void pipe_read(conn_t *conn)
{
	DWORD	bytesRead;
	BOOL	res;
	char	*buf = conn->buf;
	DWORD	priority;

	res = GetOverlappedResult(conn->handle, &conn->overlapped, &bytesRead, FALSE);
	if( res == 0 ) {
		print_event(LOG_ERR, "Read error");
		pipe_disconnect(conn);
		return;
	}

	/* a frame is a DWORD indicating priority followed by an array of char (not necessarily followed by a terminal 0 */
	priority =*((DWORD *)buf);
	buf = buf + sizeof(DWORD);
	print_event(priority,buf);

	pipe_disconnect(conn);
}

/* returns PID of the newly created process or 0 on failure */
static DWORD create_process(char * application, char * command)
{
	STARTUPINFO StartupInfo;
	PROCESS_INFORMATION ProcessInformation;
	BOOL res;

	memset(&StartupInfo,0,sizeof(STARTUPINFO));

	res = CreateProcess(
			application,
			command,
			NULL,
			NULL,
			FALSE,
			CREATE_NEW_PROCESS_GROUP,
			NULL,
			NULL,
			&StartupInfo,
			&ProcessInformation
			);

	if( res == 0 ) {
		print_event(LOG_ERR, "Can't create process");
		return 0;
	}

	return  ProcessInformation.dwProcessId;
}

/* return PID of created process or 0 on failure */
static DWORD run_drivers()
{
	char application[MAX_PATH];
	char command[MAX_PATH];

	snprintf(application,sizeof(application),"%s/upsdrvctl.exe",BINDIR);
	snprintf(command,sizeof(command),"upsdrvctl.exe start");
	return create_process(application,command);
}

/* return PID of created process or 0 on failure */
static DWORD stop_drivers()
{
	char application[MAX_PATH];
	char command[MAX_PATH];

	snprintf(application,sizeof(application),"%s/upsdrvctl.exe",BINDIR);
	snprintf(command,sizeof(command),"upsdrvctl.exe stop");
	return create_process(application,command);
}

/* return PID of created process or 0 on failure */
static void run_upsd()
{
	char application[MAX_PATH];
	char command[MAX_PATH];
	snprintf(application,sizeof(application),"%s/upsd.exe",SBINDIR);
	snprintf(command,sizeof(command),"upsd.exe");
	upsd_pid = create_process(application,command);
}

static void stop_upsd()
{
	if ( GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT,upsd_pid) == 0 ) {
		print_event(LOG_ERR, "Error sendind CTRL_BREAK to upsd");
	}
}

/* return PID of created process or 0 on failure */
static void run_upsmon()
{
	char application[MAX_PATH];
	char command[MAX_PATH];
	snprintf(application,sizeof(application),"%s/upsmon.exe",SBINDIR);
	/* FIXME "-p" is to prevent the fork of upsmon.
	Maybe this will need more investigation to avoid security breach ?? */
	snprintf(command,sizeof(command),"upsmon.exe -p");
	upsmon_pid = create_process(application,command);
}

static void stop_upsmon()
{
	if ( GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT,upsmon_pid) == 0 ) {
		print_event(LOG_ERR, "Error sendind CTRL_BREAK to upsmon");
	}
}

/* return 0 on failure */
static int parse_nutconf(BOOL start_flag)
{
	char	fn[SMALLBUF];
	FILE	*nutf;
	char	buf[SMALLBUF];

	snprintf(fn,sizeof(fn),"%s/nut.conf",CONFPATH);

	nutf = fopen(fn, "r");
	if(nutf == NULL) {
		snprintf(buf,sizeof(buf),"Error opening %s",fn);
		print_event(LOG_ERR,buf);
		return 0;
	}

	while( fgets(buf,sizeof(buf),nutf) != NULL ) {
		if(buf[0] != '#') {
			if( strstr(buf,"none") != NULL ) {
				return 1;

			}
			if( strstr(buf,"standalone") != NULL ||
					strstr(buf,"netserver") != NULL ) {
				if( start_flag == NUT_START ) {
					run_drivers();
					run_upsd();
					run_upsmon();
					return 1;
				}
				else {
					stop_upsd();
					stop_drivers();
					stop_upsmon();
					return 1;
				}
			}
			if( strstr(buf,"netclient") != NULL ) {
				if( start_flag == NUT_START ) {
					run_upsmon();
					return 1;
				}
				else {
					stop_upsmon();
					return 1;
				}
			}
		}
	}

	print_event(LOG_ERR,"No valid MODE in nut.conf");
	return 0;
}

static int SvcInstall(const char * SvcName, const char * args)
{
	SC_HANDLE SCManager;
	SC_HANDLE Service;
	TCHAR Path[MAX_PATH];

	if( !GetModuleFileName( NULL, Path, MAX_PATH ) ) {
		printf("Cannot install service (%d)\n", (int)GetLastError());
		return EXIT_FAILURE;
	}

	if( args != NULL ) {
		snprintfcat(Path, sizeof(Path), " %s", args);
	}

	SCManager = OpenSCManager(
			NULL,			/* local computer */
			NULL,			/* ServiceActive database */
			SC_MANAGER_ALL_ACCESS);	/* full access rights */

	if (NULL == SCManager) {
		upslogx(LOG_ERR, "OpenSCManager failed (%d)\n", (int)GetLastError());
		return EXIT_FAILURE;
	}

	Service = CreateService(
			SCManager,			/* SCM database */
			SvcName,			/* name of service */
			SvcName,			/* service name to display */
			SERVICE_ALL_ACCESS,		/* desired access */
			SERVICE_WIN32_OWN_PROCESS,	/* service type */
			SERVICE_DEMAND_START,		/* start type */
			SERVICE_ERROR_NORMAL,		/* error control type */
			Path,				/* path to service binary */
			NULL,				/* no load ordering group */
			NULL,				/* no tag identifier */
			NULL,				/* no dependencies */
			NULL,				/* LocalSystem account */
			NULL);				/* no password */

	if (Service == NULL) {
		upslogx(LOG_ERR, "CreateService failed (%d)\n", (int)GetLastError());
		CloseServiceHandle(SCManager);
		return EXIT_FAILURE;
	}
	else {
		upslogx(LOG_INFO, "Service installed successfully\n");
	}

	CloseServiceHandle(Service);
	CloseServiceHandle(SCManager);

	return EXIT_SUCCESS;
}

static int SvcUninstall(const char * SvcName)
{
	SC_HANDLE SCManager;
	SC_HANDLE Service;

	SCManager = OpenSCManager(
			NULL,			/* local computer */
			NULL,			/* ServicesActive database */
			SC_MANAGER_ALL_ACCESS);	/* full access rights */

	if (NULL == SCManager) {
		upslogx(LOG_ERR, "OpenSCManager failed (%d)\n", (int)GetLastError());
		return EXIT_FAILURE;
	}

	Service = OpenService(
			SCManager,	/* SCM database */
			SvcName,	/* name of service */
			DELETE);	/* need delete access */

	if (Service == NULL) {
		upslogx(LOG_ERR, "OpenService failed (%d)\n", (int)GetLastError());
		CloseServiceHandle(SCManager);
		return EXIT_FAILURE;
	}

	if (! DeleteService(Service) )  {
		upslogx(LOG_ERR,"DeleteService failed (%d)\n", (int)GetLastError());
	}
	else {
		upslogx(LOG_ERR,"Service deleted successfully\n");
	}

	CloseServiceHandle(Service);
	CloseServiceHandle(SCManager);

	return EXIT_SUCCESS;
}

static void ReportSvcStatus(   DWORD CurrentState,
		DWORD Win32ExitCode,
		DWORD WaitHint)
{
	static DWORD CheckPoint = 1;

	SvcStatus.dwCurrentState = CurrentState;
	SvcStatus.dwWin32ExitCode = Win32ExitCode;
	SvcStatus.dwWaitHint = WaitHint;

	if (CurrentState == SERVICE_START_PENDING)
		SvcStatus.dwControlsAccepted = 0;
	else SvcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

	if ( (CurrentState == SERVICE_RUNNING) ||
			(CurrentState == SERVICE_STOPPED) ) {
		SvcStatus.dwCheckPoint = 0;
	}
	else {
		SvcStatus.dwCheckPoint = CheckPoint++;
	}

	/* report the status of the service to the SCM */
	SetServiceStatus( SvcStatusHandle, &SvcStatus );
}

static void WINAPI SvcCtrlHandler( DWORD Ctrl )
{
	switch(Ctrl)
	{
		case SERVICE_CONTROL_STOP:
		case SERVICE_CONTROL_SHUTDOWN:
			ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);

			/* Signal the service to stop */
			SetEvent(svc_stop);
			ReportSvcStatus(SvcStatus.dwCurrentState, NO_ERROR, 0);

			return;

		case SERVICE_CONTROL_INTERROGATE:
			break;

		default:
			break;
	}
}

static void SvcStart(char * SvcName)
{
	/* Register the handler function for the service */
	SvcStatusHandle = RegisterServiceCtrlHandler(
			SvcName,
			SvcCtrlHandler);

	if( !SvcStatusHandle ) {
		upslogx(LOG_ERR, "RegisterServiceCtrlHandler\n");
		return;
	}

	SvcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	SvcStatus.dwServiceSpecificExitCode = 0;

	/* Report initial status to the SCM */
	ReportSvcStatus( SERVICE_START_PENDING, NO_ERROR, 3000 );
}

static void SvcReady(void)
{
	svc_stop = CreateEvent(
			NULL,	/* default security attributes */
			TRUE,	/* manual reset event */
			FALSE,	/* not signaled */
			NULL);	/* no name */

	if( svc_stop == NULL ) {
		ReportSvcStatus( SERVICE_STOPPED, NO_ERROR, 0);
		return;
	}
	ReportSvcStatus( SERVICE_RUNNING, NO_ERROR, 0);
}

static void WINAPI SvcMain( DWORD argc, LPTSTR *argv )
{

	DWORD	ret;
	HANDLE	handles[MAXIMUM_WAIT_OBJECTS];
	int	maxhandle = 0;
	conn_t	*conn;

	if(service_flag) {
		SvcStart(SVCNAME);
	}

	/* A service has no console, so do has its children. */
	/* So if we want to be able to send CTRL+BREAK signal we must */
	/* create a console which will be inheritated by children */
	AllocConsole();

	print_event(LOG_INFO,"Starting");

	/* pipe for event log proxy */
	pipe_create();

	/* parse nut.conf and start relevant processes */
	if ( parse_nutconf(NUT_START) == 0 ) {
		print_event(LOG_INFO, "exiting");
		ReportSvcStatus( SERVICE_STOPPED, NO_ERROR, 0);
		return;
	}

	if(service_flag) {
		SvcReady();
	}

	while (1) {
		maxhandle = 0;
		memset(&handles,0,sizeof(handles));

		/* Wait on the read IO of each connections */
		for (conn = connhead; conn; conn = conn->next) {
			handles[maxhandle] = conn->overlapped.hEvent;
			maxhandle++;
		}
		/* Add the new pipe connected event */
		handles[maxhandle] = pipe_connection_overlapped.hEvent;
		maxhandle++;

		/* Add SCM event handler in service mode*/
		if(service_flag) {
			handles[maxhandle] = svc_stop;
			maxhandle++;
		}

		ret = WaitForMultipleObjects(maxhandle,handles,FALSE,INFINITE);

		if (ret == WAIT_FAILED) {
			print_event(LOG_ERR, "Wait failed");
			return;
		}

		if( handles[ret] == svc_stop && service_flag ) {
			parse_nutconf(NUT_STOP);
			if(service_flag) {
				print_event(LOG_INFO, "Exiting");
				ReportSvcStatus( SERVICE_STOPPED, NO_ERROR, 0);
			}
			return;
		} 

		/* Retrieve the signaled connection */
		for(conn = connhead; conn != NULL; conn = conn->next) {
			if( conn->overlapped.hEvent == handles[ret-WAIT_OBJECT_0]) {
				break;
			}
		}
		/* a new pipe connection has been signaled */
		if (handles[ret] == pipe_connection_overlapped.hEvent) {
			pipe_connect();
		}
		/* one of the read event handle has been signaled */
		else {
			if( conn != NULL) {
				pipe_read(conn);
			}
		}
	}
}

int main(int argc, char **argv)
{
	int i;
	while ((i = getopt(argc, argv, "+IUN")) != -1) {
		switch (i) {
			case 'I':
				return SvcInstall(SVCNAME,NULL);
			case 'U':
				return SvcUninstall(SVCNAME);
			case 'N':
				service_flag = FALSE;
				break;
			default:
				break;
		}
	}

	optind = 0;

	SERVICE_TABLE_ENTRY DispatchTable[] =
	{
		{ SVCNAME, (LPSERVICE_MAIN_FUNCTION) SvcMain },
		{ NULL, NULL }
	};
	/* This call returns when the service has stopped */
	if(service_flag ) {
		if (!StartServiceCtrlDispatcher( DispatchTable ))
		{
			print_event(LOG_ERR, "StartServiceCtrlDispatcher failed : exiting, this is a Windows service which can't be run as a regular application by default. Try -N to start it as a regular application");
		}
	}
	else {
		SvcMain(argc,argv);
	}

	return EXIT_SUCCESS;
}
#endif  /* WIN32 */
