/*
 * Harbour Project source code:
 * low level functions to create, wait and terminate processes
 *
 * Copyright 2009 Przemyslaw Czerpak <druzus / at / priv.onet.pl>
 * www - http://harbour-project.org
 * based on xHarbour code by
 * Copyright 2003 Giancarlo Niccolai <gian@niccolai.ws>
 * www - http://www.xharbour.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.txt.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307 USA (or visit the web site http://www.gnu.org/).
 *
 * As a special exception, the Harbour Project gives permission for
 * additional uses of the text contained in its release of Harbour.
 *
 * The exception is that, if you link the Harbour libraries with other
 * files to produce an executable, this does not by itself cause the
 * resulting executable to be covered by the GNU General Public License.
 * Your use of that executable is in no way restricted on account of
 * linking the Harbour library code into it.
 *
 * This exception does not however invalidate any other reasons why
 * the executable file might be covered by the GNU General Public License.
 *
 * This exception applies only to the code released by the Harbour
 * Project under the name Harbour.  If you copy code from other
 * Harbour Project or Free Software Foundation releases into a copy of
 * Harbour, as the General Public License permits, the exception does
 * not apply to the code that you add in this way.  To avoid misleading
 * anyone as to the status of such modified files, you must delete
 * this exception notice from them.
 *
 * If you write modifications of your own for Harbour, it is your choice
 * whether to permit this exception to apply to your modifications.
 * If you do not wish that, delete this exception notice.
 *
 */

#include "hbapi.h"
#include "hbapifs.h"
#include "hbvm.h"

#if defined( HB_OS_UNIX )
#  include <unistd.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <signal.h>
#elif defined( HB_OS_OS2 )
#  define INCL_DOSERRORS
#  define INCL_DOSPROCESS
#  include <os2.h>
#  include <io.h>
#  include <process.h>
#  include <fcntl.h>
#  if defined( HB_OS_OS2 ) && defined( __GNUC__ )
#    include <sys/wait.h>
#  endif
#elif defined( HB_OS_DOS )
#  include <process.h>
#  include <fcntl.h>
#  if defined( __DJGPP__ )
#     include <sys/stat.h>
#     include <unistd.h>
#  else
#     include <io.h>
#  endif
#elif defined( HB_OS_WIN )
#  include <windows.h>
#  include "hbwinuni.h"
#  if defined( HB_OS_WIN_CE )
#     include "hbwince.h"
#  endif
#endif

#ifndef HB_PROCESS_USEFILES
#  if defined( HB_OS_DOS ) || defined( HB_OS_WIN_CE ) || defined( HB_OS_OS2 )
#    define HB_PROCESS_USEFILES
#  endif
#endif

#if defined( HB_OS_OS2 )

static char * hb_buildArgsOS2( const char *pszFileName )
{
   char * pArgs, * pszFree = NULL, cQuote = 0;
   HB_SIZE nLen;

   while( HB_ISSPACE( *pszFileName ) )
      ++pszFileName;

   pszFileName = hb_osEncodeCP( pszFileName, &pszFree, NULL );

   nLen = strlen( pszFileName );
   pArgs = ( char * ) hb_xgrab( nLen + 2 );
   memcpy( pArgs, pszFileName, nLen + 1 );
   pArgs[ nLen + 1 ] = '\0';

   if( pszFree )
      hb_xfree( pszFree );

   pszFree = pArgs;
   while( *pszFree )
   {
      if( *pszFree == cQuote )
         cQuote = 0;
      else if( cQuote == 0 )
      {
         if( *pszFree == '"' )
            cQuote = *pszFree;
         else if( HB_ISSPACE( *pszFree ) )
         {
            *pszFree = '\0';
            break;
         }
      }
      ++pszFree;
   }

   return pArgs;
}

#endif

#if defined( HB_OS_WIN_CE ) && defined( HB_PROCESS_USEFILES )

static void hb_getCommand( const char * pszFileName,
                           LPTSTR * lpAppName, LPTSTR * lpParams )
{
   const char * src, * params;
   char cQuote = 0;

   while( HB_ISSPACE( *pszFileName ) )
      ++pszFileName;

   params = NULL;
   src = pszFileName;
   while( *src )
   {
      if( *src == cQuote )
         cQuote = 0;
      else if( cQuote == 0 )
      {
         if( *src == '"' )
            cQuote = *src;
         else if( HB_ISSPACE( *src ) )
         {
            params = src;
            while( HB_ISSPACE( *params ) )
               ++params;
            if( *params == 0 )
               params = NULL;
            break;
         }
      }
      ++src;
   }

   *lpParams = params ? HB_CHARDUP( params ) : NULL;
   *lpAppName = HB_CHARDUPN( pszFileName, src - pszFileName );
}

#elif defined( HB_PROCESS_USEFILES ) || defined( HB_OS_UNIX )

/* convert command to argument list using standard bourne shell encoding:
 * "" and '' can be used to group parameters with blank characters,
 * the escape character is '\', quoting by '' disables escape character.
 */
static char ** hb_buildArgs( const char *pszFileName )
{
   const char * src;
   char ** argv, * dst, cQuote = 0, * pszFree = NULL;
   int argc = 0;

   while( HB_ISSPACE( *pszFileName ) )
      ++pszFileName;

   pszFileName = hb_osEncodeCP( pszFileName, &pszFree, NULL );
   dst = pszFree ? pszFree : hb_strdup( pszFileName );

   src = dst;
   while( *src )
   {
#if defined( HB_OS_UNIX )
      if( *src == '\\' && cQuote != '\'' )
      {
         if( src[ 1 ] )
            ++src;
      }
      else
#endif
      if( *src == cQuote )
         cQuote = 0;
      else if( cQuote == 0 )
      {
#if defined( HB_OS_UNIX )
         if( *src == '"' || *src == '\'' )
#else
         if( *src == '"' )
#endif
            cQuote = *src;
         else if( HB_ISSPACE( *src ) )
         {
            while( HB_ISSPACE( src[ 1 ] ) )
               ++src;
            if( src[ 1 ] )
               ++argc;
         }
      }
      ++src;
   }

   argv = ( char ** ) hb_xgrab( ( argc + 2 ) * sizeof( char * ) );
   argv[ 0 ] = dst;
   argv[ argc + 1 ] = NULL;
   argc = 0;

   cQuote = 0;
   src = dst;
   while( *src )
   {
#if defined( HB_OS_UNIX )
      if( *src == '\\' && cQuote != '\'' )
      {
         if( src[ 1 ] )
         {
            *dst++ = src[ 1 ];
            ++src;
         }
      }
      else
#endif
      if( *src == cQuote )
         cQuote = 0;
      else if( cQuote != 0 )
         *dst++ = *src;
      else
      {
#if defined( HB_OS_UNIX )
         if( *src == '"' || *src == '\'' )
#else
         if( *src == '"' )
#endif
            cQuote = *src;
         else if( HB_ISSPACE( *src ) )
         {
            *dst++ = '\0';
            while( HB_ISSPACE( src[ 1 ] ) )
               ++src;
            if( src[ 1 ] )
               argv[ ++argc ] = dst;
         }
         else
            *dst++ = *src;
      }
      ++src;
   }
   *dst = 0;

   return argv;
}

static void hb_freeArgs( char ** argv )
{
   hb_xfree( argv[ 0 ] );
   hb_xfree( argv );
}

#endif

#if defined( HB_PROCESS_USEFILES )
static int hb_fsProcessExec( const char * pszFileName,
                             HB_FHANDLE hStdin, HB_FHANDLE hStdout,
                             HB_FHANDLE hStderr )
{
   int iResult = FS_ERROR;

   HB_TRACE( HB_TR_DEBUG, ( "hb_fsProcessExec(%s, %p, %p, %p)", pszFileName, ( void * ) ( HB_PTRDIFF ) hStdin, ( void * ) ( HB_PTRDIFF ) hStdout, ( void * ) ( HB_PTRDIFF ) hStderr ) );

#if defined( HB_OS_WIN_CE )
{
   LPTSTR lpAppName, lpParams;
   HB_BOOL fError;

   HB_SYMBOL_UNUSED( hStdin );
   HB_SYMBOL_UNUSED( hStdout );
   HB_SYMBOL_UNUSED( hStderr );

   hb_getCommand( pszFileName, &lpAppName, &lpParams );

   hb_vmUnlock();
   fError = ! CreateProcess( lpAppName,      /* lpAppName */
                             lpParams,       /* lpCommandLine */
                             NULL,           /* lpProcessAttr */
                             NULL,           /* lpThreadAttr */
                             FALSE,          /* bInheritHandles */
                             0,              /* dwCreationFlags */
                             NULL,           /* lpEnvironment */
                             NULL,           /* lpCurrentDirectory */
                             NULL,           /* lpStartupInfo */
                             NULL );         /* lpProcessInformation */
   hb_fsSetIOError( ! fError, 0 );
   if( ! fError )
   {
      hb_fsSetIOError( ! fError, 0 );
      iResult = 0;
   }
   hb_vmLock();

   if( lpAppName )
      hb_xfree( lpAppName );
   if( lpParams )
      hb_xfree( lpParams );
}
#elif defined( HB_OS_DOS ) || defined( HB_OS_WIN ) || defined( HB_OS_OS2 ) || \
      defined( HB_OS_UNIX )
{
   int iStdIn, iStdOut, iStdErr;
   char ** argv;

   argv = hb_buildArgs( pszFileName );

   hb_vmUnlock();

   iStdIn = iStdOut = iStdErr = FS_ERROR;

   if( hStdin != FS_ERROR )
   {
      iStdIn  = dup( 0 );
      dup2( hStdin,  0 );
   }
   if( hStdout != FS_ERROR )
   {
      iStdOut = dup( 1 );
      dup2( hStdout, 1 );
   }
   if( hStderr != FS_ERROR )
   {
      iStdErr = dup( 2 );
      dup2( hStderr, 2 );
   }
#if defined( HB_OS_UNIX ) && ! defined( HB_OS_VXWORKS ) && ! defined( HB_OS_SYMBIAN )
   {
      pid_t pid = fork();
      if( pid == 0 )
      {
         /* close all non std* handles */
         {
            int iMaxFD, i;
            iMaxFD = sysconf( _SC_OPEN_MAX );
            if( iMaxFD < 3 )
               iMaxFD = 1024;
            for( i = 3; i < iMaxFD; ++i )
               hb_fsClose( i );
         }
         /* reset extended process attributes */
         ( void ) setuid( getuid() );
         ( void ) setgid( getgid() );

         /* execute command */
         execvp( argv[ 0 ], argv );
         exit( -1 );
      }
      else if( pid != -1 )
      {
         int iStatus;
         iResult = waitpid( pid, &iStatus, 0 );
#ifdef ERESTARTSYS
         if( iResult < 0 && errno != ERESTARTSYS )
#else
         if( iResult < 0 )
#endif
            iResult = -2;
         else if( iResult == 0 )
            iResult = -1;
         else
            iResult = WIFEXITED( iStatus ) ? WEXITSTATUS( iStatus ) : 0;
      }
   }
#elif defined( _MSC_VER ) || defined( __LCC__ ) || \
    defined( __XCC__ ) || defined( __POCC__ )
   iResult = _spawnvp( _P_WAIT, argv[ 0 ], argv );
#elif defined( __MINGW32__ ) || defined( __WATCOMC__ )
   iResult = spawnvp( P_WAIT, argv[ 0 ], ( const char * const * ) argv );
#else
   iResult = spawnvp( P_WAIT, argv[ 0 ], ( char * const * ) argv );
#endif
   hb_fsSetIOError( iResult >= 0, 0 );

   if( iStdIn != FS_ERROR )
   {
      dup2( iStdIn,  0 );
      close( iStdIn );
   }
   if( iStdOut != FS_ERROR )
   {
      dup2( iStdOut, 1 );
      close( iStdOut );
   }
   if( iStdErr != FS_ERROR )
   {
      dup2( iStdErr, 2 );
      close( iStdErr );
   }

   hb_vmLock();
   hb_freeArgs( argv );
}
#else
{
   int iTODO; /* TODO: for given platform */

   HB_SYMBOL_UNUSED( pszFileName );
   HB_SYMBOL_UNUSED( hStdin );
   HB_SYMBOL_UNUSED( hStdout );
   HB_SYMBOL_UNUSED( hStderr );

   hb_fsSetError( ( HB_ERRCODE ) FS_ERROR );
}
#endif

   return iResult;
}
#endif

HB_FHANDLE hb_fsProcessOpen( const char * pszFileName,
                             HB_FHANDLE * phStdin, HB_FHANDLE * phStdout,
                             HB_FHANDLE * phStderr,
                             HB_BOOL fDetach, HB_ULONG * pulPID )
{
   HB_FHANDLE hPipeIn [ 2 ] = { FS_ERROR, FS_ERROR },
              hPipeOut[ 2 ] = { FS_ERROR, FS_ERROR },
              hPipeErr[ 2 ] = { FS_ERROR, FS_ERROR };
   HB_FHANDLE hResult = FS_ERROR;
   HB_BOOL fError = HB_FALSE;

   HB_TRACE( HB_TR_DEBUG, ( "hb_fsProcessOpen(%s, %p, %p, %p, %d, %p)", pszFileName, phStdin, phStdout, phStderr, fDetach, pulPID ) );

   if( phStdin != NULL )
      fError = ! hb_fsPipeCreate( hPipeIn );
   if( ! fError && phStdout != NULL )
      fError = ! hb_fsPipeCreate( hPipeOut );
   if( ! fError && phStderr != NULL )
   {
      if( phStdout == phStderr )
      {
         hPipeErr[ 0 ] = hPipeOut[ 0 ];
         hPipeErr[ 1 ] = hPipeOut[ 1 ];
      }
      else
         fError = ! hb_fsPipeCreate( hPipeErr );
   }

   if( ! fError )
   {
#if defined( HB_OS_WIN )

      PROCESS_INFORMATION pi;
      STARTUPINFO si;
      DWORD dwFlags = 0;
      LPTSTR lpCommand = HB_CHARDUP( pszFileName );

#  if ! defined( HB_OS_WIN_CE )
      if( phStdin != NULL )
         SetHandleInformation( ( HANDLE ) hb_fsGetOsHandle( hPipeIn [ 1 ] ), HANDLE_FLAG_INHERIT, 0 );
      if( phStdout != NULL )
         SetHandleInformation( ( HANDLE ) hb_fsGetOsHandle( hPipeOut[ 0 ] ), HANDLE_FLAG_INHERIT, 0 );
      if( phStderr != NULL && phStdout != phStderr )
         SetHandleInformation( ( HANDLE ) hb_fsGetOsHandle( hPipeErr[ 0 ] ), HANDLE_FLAG_INHERIT, 0 );
#  endif

      memset( &pi, 0, sizeof( pi ) );
      memset( &si, 0, sizeof( si ) );
      si.cb = sizeof( si );
#  ifdef STARTF_USESTDHANDLES
      si.dwFlags = STARTF_USESTDHANDLES;
#  endif
      if( fDetach )
      {
#  ifdef STARTF_USESHOWWINDOW
         si.dwFlags |= STARTF_USESHOWWINDOW;
#  endif
         si.wShowWindow = SW_HIDE;
         si.hStdInput  = ( HANDLE ) hb_fsGetOsHandle( hPipeIn [ 0 ] );
         si.hStdOutput = ( HANDLE ) hb_fsGetOsHandle( hPipeOut[ 1 ] );
         si.hStdError  = ( HANDLE ) hb_fsGetOsHandle( hPipeErr[ 1 ] );
#  ifdef DETACHED_PROCESS
         dwFlags |= DETACHED_PROCESS;
#  endif
      }
      else
      {
         si.hStdInput  = phStdin  ? ( HANDLE ) hb_fsGetOsHandle( hPipeIn [ 0 ] ) : GetStdHandle( STD_INPUT_HANDLE );
         si.hStdOutput = phStdout ? ( HANDLE ) hb_fsGetOsHandle( hPipeOut[ 1 ] ) : GetStdHandle( STD_OUTPUT_HANDLE );
         si.hStdError  = phStderr ? ( HANDLE ) hb_fsGetOsHandle( hPipeErr[ 1 ] ) : GetStdHandle( STD_ERROR_HANDLE );
      }
      fError = ! CreateProcess( NULL,           /* lpAppName */
                                lpCommand,
                                NULL,           /* lpProcessAttr */
                                NULL,           /* lpThreadAttr */
                                TRUE,           /* bInheritHandles */
                                dwFlags,        /* dwCreationFlags */
                                NULL,           /* lpEnvironment */
                                NULL,           /* lpCurrentDirectory */
                                &si,
                                &pi );
      hb_fsSetIOError( ! fError, 0 );
      hb_xfree( lpCommand );
      if( ! fError )
      {
         if( phStdin != NULL )
         {
            *phStdin = ( HB_FHANDLE ) hPipeIn[ 1 ];
            hPipeIn[ 1 ] = FS_ERROR;
         }
         if( phStdout != NULL )
         {
            *phStdout = ( HB_FHANDLE ) hPipeOut[ 0 ];
            hPipeOut[ 0 ] = FS_ERROR;
         }
         if( phStderr != NULL )
         {
            *phStderr = ( HB_FHANDLE ) hPipeErr[ 0 ];
            hPipeErr[ 0 ] = FS_ERROR;
         }
         if( pulPID )
            *pulPID = pi.dwProcessId;
         CloseHandle( pi.hThread );
         hResult = ( HB_FHANDLE ) pi.hProcess;
      }

#elif defined( HB_OS_OS2 )

      HFILE hStdIn  = ( HFILE ) FS_ERROR,
            hStdErr = ( HFILE ) FS_ERROR,
            hStdOut = ( HFILE ) FS_ERROR,
            hNull   = ( HFILE ) FS_ERROR,
            hDup;
      APIRET ret = NO_ERROR;
      ULONG ulState, ulStateIn, ulStateOut, ulStateErr;
      PID pid = ( PID ) -1;

      ulStateIn = ulStateOut = ulStateErr = OPEN_FLAGS_NOINHERIT;
      ulState = 0;
      if( fDetach && ( ! phStdin || ! phStdout || ! phStderr ) )
         ret = DosOpen( ( PCSZ ) "NUL:", &hNull, &ulState, 0,
                        FILE_NORMAL, OPEN_ACCESS_READWRITE,
                        OPEN_ACTION_OPEN_IF_EXISTS, NULL );

      if( ret == NO_ERROR && phStdin != NULL )
      {
         ret = DosQueryFHState( hPipeIn[ 1 ], &ulState );
         if( ret == NO_ERROR && ( ulState & OPEN_FLAGS_NOINHERIT ) == 0 )
            ret = DosSetFHState( hPipeIn[ 1 ], ( ulState & 0xFF00 ) | OPEN_FLAGS_NOINHERIT );
      }
      if( ret == NO_ERROR && phStdout != NULL )
      {
         ret = DosQueryFHState( hPipeOut[ 0 ], &ulState);
         if( ret == NO_ERROR && ( ulState & OPEN_FLAGS_NOINHERIT ) == 0 )
            ret = DosSetFHState( hPipeOut[ 0 ], ( ulState & 0xFF00 ) | OPEN_FLAGS_NOINHERIT );
      }
      if( ret == NO_ERROR && phStderr != NULL && phStdout != phStderr )
      {
         ret = DosQueryFHState( hPipeErr[ 0 ], &ulState);
         if( ret == NO_ERROR && ( ulState & OPEN_FLAGS_NOINHERIT ) == 0 )
            ret = DosSetFHState( hPipeErr[ 0 ], ( ulState & 0xFF00 ) | OPEN_FLAGS_NOINHERIT );
      }

      if( ret == NO_ERROR && ( phStdin != NULL || fDetach ) )
      {
         hDup = 0;
         ret = DosDupHandle( hDup, &hStdIn );
         if( ret == NO_ERROR )
         {
            ret = DosQueryFHState( hStdIn, &ulStateIn );
            if( ret == NO_ERROR && ( ulStateIn & OPEN_FLAGS_NOINHERIT ) == 0 )
               ret = DosSetFHState( hStdIn, ( ulStateIn & 0xFF00 ) | OPEN_FLAGS_NOINHERIT );
            if( ret == NO_ERROR )
               ret = DosDupHandle( phStdin != NULL ? ( HFILE ) hPipeIn[ 0 ] : hNull, &hDup );
         }
      }

      if( ret == NO_ERROR && ( phStdout != NULL || fDetach ) )
      {
         hDup = 1;
         ret = DosDupHandle( hDup, &hStdOut );
         if( ret == NO_ERROR )
         {
            ret = DosQueryFHState( hStdOut, &ulStateOut );
            if( ret == NO_ERROR && ( ulStateOut & OPEN_FLAGS_NOINHERIT ) == 0 )
               ret = DosSetFHState( hStdOut, ( ulStateOut & 0xFF00 ) | OPEN_FLAGS_NOINHERIT );
            if( ret == NO_ERROR )
               ret = DosDupHandle( phStdout != NULL ? ( HFILE ) hPipeOut[ 1 ] : hNull, &hDup );
         }
      }

      if( ret == NO_ERROR && ( phStderr != NULL || fDetach ) )
      {
         hDup = 2;
         ret = DosDupHandle( hDup, &hStdErr );
         if( ret == NO_ERROR )
         {
            ret = DosQueryFHState( hStdErr, &ulStateErr );
            if( ret == NO_ERROR && ( ulStateErr & OPEN_FLAGS_NOINHERIT ) == 0 )
               ret = DosSetFHState( hStdErr, ( ulStateErr & 0xFF00 ) | OPEN_FLAGS_NOINHERIT );
            if( ret == NO_ERROR )
               ret = DosDupHandle( phStderr != NULL ? ( HFILE ) hPipeErr[ 1 ] : hNull, &hDup );
         }
      }

      if( ret == NO_ERROR )
      {
         char * pArgs = hb_buildArgsOS2( pszFileName );
         char uchLoadError[ CCHMAXPATH ] = { 0 };
         RESULTCODES ChildRC = { 0, 0 };

         ret = DosExecPgm( uchLoadError, sizeof( uchLoadError ),
                           fDetach ? EXEC_BACKGROUND : EXEC_ASYNCRESULT,
                           ( PCSZ ) pArgs, NULL /* env */,
                           &ChildRC,
                           ( PCSZ ) pArgs );
         if( ret == NO_ERROR )
            pid = ChildRC.codeTerminate;

         hb_xfree( pArgs );
      }

      if( hNull != ( HFILE ) FS_ERROR )
         DosClose( hNull );

      if( hStdIn != ( HFILE ) FS_ERROR )
      {
         hDup = 0;
         DosDupHandle( hStdIn, &hDup );
         DosClose( hStdIn );
         if( ( ulStateIn & OPEN_FLAGS_NOINHERIT ) == 0 )
            DosSetFHState( hDup, ulStateIn & 0xFF00 );
      }
      if( hStdOut != ( HFILE ) FS_ERROR )
      {
         hDup = 1;
         DosDupHandle( hStdOut, &hDup );
         DosClose( hStdOut );
         if( ( ulStateOut & OPEN_FLAGS_NOINHERIT ) == 0 )
            DosSetFHState( hDup, ulStateOut & 0xFF00 );
      }
      if( hStdErr != ( HFILE ) FS_ERROR )
      {
         hDup = 2;
         DosDupHandle( hStdErr, &hDup );
         DosClose( hStdErr );
         if( ( ulStateErr & OPEN_FLAGS_NOINHERIT ) == 0 )
            DosSetFHState( hDup, ulStateErr & 0xFF00 );
      }

      fError = ret != NO_ERROR;
      if( ! fError )
      {
         if( phStdin != NULL )
         {
            *phStdin = ( HB_FHANDLE ) hPipeIn[ 1 ];
            hPipeIn[ 1 ] = FS_ERROR;
         }
         if( phStdout != NULL )
         {
            *phStdout = ( HB_FHANDLE ) hPipeOut[ 0 ];
            hPipeOut[ 0 ] = FS_ERROR;
         }
         if( phStderr != NULL )
         {
            *phStderr = ( HB_FHANDLE ) hPipeErr[ 0 ];
            hPipeErr[ 0 ] = FS_ERROR;
         }
         if( pulPID )
            *pulPID = pid;
         hResult = ( HB_FHANDLE ) pid;
      }

      hb_fsSetError( ( HB_ERRCODE ) ret );

#elif defined( HB_OS_UNIX ) && \
      ! defined( HB_OS_VXWORKS ) && ! defined( HB_OS_SYMBIAN )

      char ** argv = hb_buildArgs( pszFileName );
      pid_t pid = fork();

      if( pid == -1 )
         fError = HB_TRUE;
      else if( pid != 0 )    /* parent process */
      {
         if( phStdin != NULL )
         {
            *phStdin = ( HB_FHANDLE ) hPipeIn[ 1 ];
            hPipeIn[ 1 ] = FS_ERROR;
         }
         if( phStdout != NULL )
         {
            *phStdout = ( HB_FHANDLE ) hPipeOut[ 0 ];
            hPipeOut[ 0 ] = FS_ERROR;
         }
         if( phStderr != NULL )
         {
            *phStderr = ( HB_FHANDLE ) hPipeErr[ 0 ];
            hPipeErr[ 0 ] = FS_ERROR;
         }
         if( pulPID )
            *pulPID = pid;
         hResult = ( HB_FHANDLE ) pid;
      }
      else                    /* child process */
      {
         if( fDetach && ( ! phStdin || ! phStdout || ! phStderr ) )
         {
            HB_FHANDLE hNull = open( "/dev/null", O_RDWR );

            if( ! phStdin )
               dup2( hNull, 0 );
            if( ! phStdout )
               dup2( hNull, 1 );
            if( ! phStderr )
               dup2( hNull, 2 );

            if( hNull != FS_ERROR )
               hb_fsClose( hNull );
         }

         if( phStdin != NULL )
         {
            dup2( hPipeIn[ 0 ], 0 );
            hb_fsClose( hPipeIn[ 1 ] );
         }
         if( phStdout != NULL )
         {
            dup2( hPipeOut[ 1 ], 1 );
            hb_fsClose( hPipeOut[ 0 ] );
         }
         if( phStderr != NULL )
         {
            dup2( hPipeErr[ 1 ], 2 );
            if( phStdout != phStderr )
               hb_fsClose( hPipeErr[ 0 ] );
         }

         /* close all non std* handles */
         {
            int iMaxFD, i;
            iMaxFD = sysconf( _SC_OPEN_MAX );
            if( iMaxFD < 3 )
               iMaxFD = 1024;
            for( i = 3; i < iMaxFD; ++i )
               hb_fsClose( i );
         }

         /* reset extended process attributes */
         if( setuid( getuid() ) == -1 ) {}
         if( setgid( getgid() ) == -1 ) {}

         /* execute command */
         {
#  if defined( __WATCOMC__ )
            execvp( argv[ 0 ], ( const char ** ) argv );
#  else
            execvp( argv[ 0 ], argv );
#  endif
            exit( -1 );
         }
      }
      hb_fsSetIOError( ! fError, 0 );

      hb_freeArgs( argv );

#elif defined( HB_OS_OS2 ) || defined( HB_OS_WIN )

      int hStdIn, hStdOut, hStdErr;
      char ** argv;
      int pid;

      hStdIn  = dup( 0 );
      hStdOut = dup( 1 );
      hStdErr = dup( 2 );

      if( fDetach && ( ! phStdin || ! phStdout || ! phStderr ) )
      {
         HB_FHANDLE hNull = open( "NUL:", O_RDWR );

         if( ! phStdin )
            dup2( hNull, 0 );
         if( ! phStdout )
            dup2( hNull, 1 );
         if( ! phStderr )
            dup2( hNull, 2 );

         if( hNull != FS_ERROR )
            close( hNull );
      }

      if( phStdin != NULL )
         dup2( hPipeIn[ 0 ], 0 );
      if( phStdout != NULL )
         dup2( hPipeOut[ 1 ], 1 );
      if( phStderr != NULL )
         dup2( hPipeErr[ 1 ], 2 );

      argv = hb_buildArgs( pszFileName );

#if defined( _MSC_VER ) || defined( __LCC__ ) || \
    defined( __XCC__ ) || defined( __POCC__ )
      pid = _spawnvp( _P_NOWAIT, argv[ 0 ], argv );
#elif defined( __MINGW32__ ) || defined( __WATCOMC__ )
      pid = spawnvp( P_NOWAIT, argv[ 0 ], ( const char * const * ) argv );
#else
      pid = spawnvp( P_NOWAIT, argv[ 0 ], ( char * const * ) argv );
#endif
      hb_freeArgs( argv );

      dup2( hStdIn,  0 );
      close( hStdIn );

      dup2( hStdOut, 1 );
      close( hStdOut );

      dup2( hStdErr, 2 );
      close( hStdErr );

      if( pid < 0 )
         fError = HB_TRUE;
      else if( pid != 0 )    /* parent process */
      {
         if( phStdin != NULL )
         {
            *phStdin = ( HB_FHANDLE ) hPipeIn[ 1 ];
            hPipeIn[ 1 ] = FS_ERROR;
         }
         if( phStdout != NULL )
         {
            *phStdout = ( HB_FHANDLE ) hPipeOut[ 0 ];
            hPipeOut[ 0 ] = FS_ERROR;
         }
         if( phStderr != NULL )
         {
            *phStderr = ( HB_FHANDLE ) hPipeErr[ 0 ];
            hPipeErr[ 0 ] = FS_ERROR;
         }
         if( pulPID )
            *pulPID = pid;
         hResult = ( HB_FHANDLE ) pid;
      }

      hb_fsSetIOError( ! fError, 0 );

#else
   int iTODO; /* TODO: for given platform */

   HB_SYMBOL_UNUSED( pszFileName );
   HB_SYMBOL_UNUSED( fDetach );
   HB_SYMBOL_UNUSED( pulPID );

   hb_fsSetError( ( HB_ERRCODE ) FS_ERROR );
#endif
   }

   if( hPipeIn[ 0 ] != FS_ERROR )
      hb_fsClose( hPipeIn[ 0 ] );
   if( hPipeIn[ 1 ] != FS_ERROR )
      hb_fsClose( hPipeIn[ 1 ] );
   if( hPipeOut[ 0 ] != FS_ERROR )
      hb_fsClose( hPipeOut[ 0 ] );
   if( hPipeOut[ 1 ] != FS_ERROR )
      hb_fsClose( hPipeOut[ 1 ] );
   if( phStdout != phStderr )
   {
      if( hPipeErr[ 0 ] != FS_ERROR )
         hb_fsClose( hPipeErr[ 0 ] );
      if( hPipeErr[ 1 ] != FS_ERROR )
         hb_fsClose( hPipeErr[ 1 ] );
   }

   return hResult;
}

int hb_fsProcessValue( HB_FHANDLE hProcess, HB_BOOL fWait )
{
   int iRetStatus = -1;

   HB_TRACE( HB_TR_DEBUG, ( "hb_fsProcessValue(%p, %d)", ( void * ) ( HB_PTRDIFF ) hProcess, fWait ) );

#if defined( HB_OS_WIN )
{
   HB_BOOL fError = HB_TRUE;
   DWORD dwResult;
   HANDLE hProc = ( HANDLE ) hb_fsGetOsHandle( hProcess );

   if( hProc )
   {
      hb_vmUnlock();
      dwResult = WaitForSingleObject( hProc, fWait ? INFINITE : 0 );
      if( dwResult == WAIT_OBJECT_0 )
      {
         fError = ! GetExitCodeProcess( hProc, &dwResult );
         iRetStatus = ! fError ? ( int ) dwResult : -2;
      }
      hb_fsSetIOError( ! fError, 0 );
      if( ! fError )
         CloseHandle( hProc );
      hb_vmLock();
   }
   else
      hb_fsSetError( ( HB_ERRCODE ) FS_ERROR );
}
#elif defined( HB_OS_OS2 )
{
   PID pid = ( PID ) hProcess;

   if( pid > 0 )
   {
      RESULTCODES resultCodes = { 0, 0 };
      APIRET ret;

      hb_vmUnlock();
      ret = DosWaitChild( DCWA_PROCESS, fWait ? DCWW_WAIT : DCWW_NOWAIT,
                          &resultCodes, &pid, pid );
      hb_fsSetError( ( HB_ERRCODE ) ret );
      if( ret == NO_ERROR )
         iRetStatus = resultCodes.codeResult;
      else
         iRetStatus = -2;
      hb_vmLock();
   }
   else
      hb_fsSetError( ( HB_ERRCODE ) FS_ERROR );
}
#elif defined( HB_OS_UNIX )
{
   int iStatus;
   pid_t pid = ( pid_t ) hProcess;

   if( pid > 0 )
   {
      hb_vmUnlock();
      iRetStatus = waitpid( pid, &iStatus, fWait ? 0 : WNOHANG );
      hb_fsSetIOError( iRetStatus >= 0, 0 );
#ifdef ERESTARTSYS
      if( iRetStatus < 0 && hb_fsOsError() != ( HB_ERRCODE ) ERESTARTSYS )
#else
      if( iRetStatus < 0 )
#endif
         iRetStatus = -2;
      else if( iRetStatus == 0 )
         iRetStatus = -1;
      else
         iRetStatus = WIFEXITED( iStatus ) ? WEXITSTATUS( iStatus ) : 0;
      hb_vmLock();
   }
   else
      hb_fsSetError( ( HB_ERRCODE ) FS_ERROR );
}
#else
{
   int iTODO; /* TODO: for given platform */

   HB_SYMBOL_UNUSED( hProcess );
   HB_SYMBOL_UNUSED( fWait );
   hb_fsSetError( ( HB_ERRCODE ) FS_ERROR );
}
#endif
   return iRetStatus;
}

/* Closes/kills process. The handle is still valid until you
 * catch it with hb_fsProcessValue.
 */
HB_BOOL hb_fsProcessClose( HB_FHANDLE hProcess, HB_BOOL fGentle )
{
   HB_BOOL fResult = HB_FALSE;

   HB_TRACE( HB_TR_DEBUG, ( "hb_fsProcessClose(%p, %d)", ( void * ) ( HB_PTRDIFF ) hProcess, fGentle ) );

#if defined( HB_OS_WIN )
{
   HANDLE hProc = ( HANDLE ) hb_fsGetOsHandle( hProcess );

   if( hProc )
   {
      fResult = TerminateProcess( hProc, fGentle ? 0 : 1 ) != 0;
      hb_fsSetIOError( fResult, 0 );
      /* hProc has to be closed by hb_fsProcessValue() */
      /* CloseHandle( hProc ); */
   }
   else
      hb_fsSetError( ( HB_ERRCODE ) FS_ERROR );
}
#elif defined( HB_OS_OS2 )
{
   PID pid = ( PID ) hProcess;

   if( pid > 0 )
   {
      APIRET ret = DosKillProcess( fGentle ? DKP_PROCESS : DKP_PROCESSTREE, pid );

      fResult = ret == NO_ERROR;
      hb_fsSetError( ( HB_ERRCODE ) ret );
   }
   else
      hb_fsSetError( ( HB_ERRCODE ) FS_ERROR );
}
#elif defined( HB_OS_UNIX ) && ! defined( HB_OS_SYMBIAN )
{
   pid_t pid = ( pid_t ) hProcess;
   if( pid > 0 )
   {
      fResult = kill( pid, fGentle ? SIGTERM : SIGKILL ) == 0;
      hb_fsSetIOError( fResult, 0 );
   }
   else
      hb_fsSetError( ( HB_ERRCODE ) FS_ERROR );
}
#else
{
   int iTODO; /* TODO: for given platform */

   HB_SYMBOL_UNUSED( hProcess );
   HB_SYMBOL_UNUSED( fGentle );
   hb_fsSetError( ( HB_ERRCODE ) FS_ERROR );
}
#endif
   return fResult;
}

#define HB_STD_BUFFER_SIZE    4096

int hb_fsProcessRun( const char * pszFileName,
                     const char * pStdInBuf, HB_SIZE nStdInLen,
                     char ** pStdOutPtr, HB_SIZE * pulStdOut,
                     char ** pStdErrPtr, HB_SIZE * pulStdErr,
                     HB_BOOL fDetach )
{
   HB_FHANDLE hStdin, hStdout, hStderr, *phStdin, *phStdout, *phStderr;
   char * pOutBuf, *pErrBuf;
   HB_SIZE nOutSize, nErrSize, nOutBuf, nErrBuf;
   int iResult;

   HB_TRACE( HB_TR_DEBUG, ( "hb_fsProcessRun(%s, %p, %" HB_PFS "u, %p, %p, %p, %p, %d)", pStdInBuf, pStdInBuf, nStdInLen, pStdOutPtr, pulStdOut, pStdErrPtr, pulStdErr, fDetach ) );

   nOutBuf = nErrBuf = nOutSize = nErrSize = 0;
   pOutBuf = pErrBuf = NULL;
   hStdin = hStdout = hStderr = FS_ERROR;
   phStdin = pStdInBuf ? &hStdin : NULL;
   phStdout = pStdOutPtr && pulStdOut ? &hStdout : NULL;
   phStderr = pStdErrPtr && pulStdErr ?
              ( pStdOutPtr == pStdErrPtr ? phStdout : &hStderr ) : NULL;

#if defined( HB_PROCESS_USEFILES )
{

#if defined( HB_OS_WIN_CE )
#  define _HB_NULLHANDLE()    FS_ERROR
#elif defined( HB_OS_UNIX )
#  define _HB_NULLHANDLE()    open( "/dev/null", O_RDWR )
#else
#  define _HB_NULLHANDLE()    open( "NUL:", O_RDWR )
#endif
   char sTmpIn[ HB_PATH_MAX ];
   char sTmpOut[ HB_PATH_MAX ];
   char sTmpErr[ HB_PATH_MAX ];

   HB_SYMBOL_UNUSED( phStdin );
   HB_SYMBOL_UNUSED( nOutSize );
   HB_SYMBOL_UNUSED( nErrSize );

   sTmpIn[ 0 ] = sTmpOut[ 0 ] = sTmpErr[ 0 ] = '\0';
   if( pStdInBuf )
   {
      hStdin = hb_fsCreateTempEx( sTmpIn, NULL, NULL, NULL, FC_NORMAL );
      if( nStdInLen )
      {
         hb_fsWriteLarge( hStdin, pStdInBuf, nStdInLen );
         hb_fsSeek( hStdin, 0, FS_SET );
      }
   }
   else if( fDetach )
      hStdin = _HB_NULLHANDLE();

   if( pStdOutPtr && pulStdOut )
      hStdout = hb_fsCreateTempEx( sTmpOut, NULL, NULL, NULL, FC_NORMAL );
   else if( fDetach )
      hStdout = _HB_NULLHANDLE();

   if( pStdErrPtr && pulStdErr )
   {
      if( phStdout == phStderr )
         hStderr = hStdout;
      else
         hStderr = hb_fsCreateTempEx( sTmpErr, NULL, NULL, NULL, FC_NORMAL );
   }
   else if( fDetach )
      hStderr = _HB_NULLHANDLE();

   iResult = hb_fsProcessExec( pszFileName, hStdin, hStdout, hStderr );

   if( hStdin != FS_ERROR )
   {
      hb_fsClose( hStdin );
      if( sTmpIn[ 0 ] )
         hb_fsDelete( sTmpIn );
   }
   if( hStdout != FS_ERROR )
   {
      if( pStdOutPtr && pulStdOut )
      {
         nOutBuf = hb_fsSeek( hStdout, 0, FS_END );
         if( nOutBuf )
         {
            pOutBuf = ( char * ) hb_xgrab( nOutBuf + 1 );
            hb_fsSeek( hStdout, 0, FS_SET );
            nOutBuf = hb_fsReadLarge( hStdout, pOutBuf, nOutBuf );
         }
      }
      hb_fsClose( hStdout );
      if( sTmpOut[ 0 ] )
         hb_fsDelete( sTmpOut );
   }
   if( hStderr != FS_ERROR && hStderr != hStdout )
   {
      if( pStdErrPtr && pulStdErr )
      {
         nErrBuf = hb_fsSeek( hStderr, 0, FS_END );
         if( nErrBuf )
         {
            pErrBuf = ( char * ) hb_xgrab( nErrBuf + 1 );
            hb_fsSeek( hStderr, 0, FS_SET );
            nErrBuf = hb_fsReadLarge( hStderr, pErrBuf, nErrBuf );
         }
      }
      hb_fsClose( hStderr );
      if( sTmpErr[ 0 ] )
         hb_fsDelete( sTmpErr );
   }
}

#else
{
   HB_FHANDLE hProcess;

   hb_vmUnlock();

   iResult = -1;
   hProcess = hb_fsProcessOpen( pszFileName, phStdin, phStdout, phStderr,
                                fDetach, NULL );
   if( hProcess != FS_ERROR )
   {
#if defined( HB_OS_WIN )

      HB_BOOL fFinished = HB_FALSE, fBlocked;
      int iPipeCount = 0;

      if( nStdInLen == 0 && hStdin != FS_ERROR )
      {
         hb_fsClose( hStdin );
         hStdin = FS_ERROR;
      }
      if( hStdout == hStderr )
         hStderr = FS_ERROR;

      if( hStdin != FS_ERROR )
         ++iPipeCount;
      if( hStdout != FS_ERROR )
         ++iPipeCount;
      if( hStderr != FS_ERROR )
         ++iPipeCount;

      fBlocked = iPipeCount <= 1;
      if( ! fBlocked )
      {
         if( hStdin != FS_ERROR )
            hb_fsPipeUnblock( hStdin );
         if( hStdout != FS_ERROR )
            hb_fsPipeUnblock( hStdout );
         if( hStderr != FS_ERROR )
            hb_fsPipeUnblock( hStderr );
      }

      for( ;; )
      {
         DWORD dwResult, dwWait;
         HB_SIZE nLen;

         dwWait = 1000;

         if( hStdout != FS_ERROR )
         {
            if( nOutBuf == nOutSize )
            {
               if( nOutSize == 0 )
                  nOutSize = HB_STD_BUFFER_SIZE;
               else
                  nOutSize += nOutSize >> 1;
               pOutBuf = ( char * ) hb_xrealloc( pOutBuf, nOutSize + 1 );
            }
            nLen = hb_fsReadLarge( hStdout, pOutBuf + nOutBuf, nOutSize - nOutBuf );
            if( nLen > 0 )
               nOutBuf += nLen;
            else if( fBlocked )
            {
               hb_fsClose( hStdout );
               hStdout = FS_ERROR;
               --iPipeCount;
            }
            dwWait = nLen > 0 ? 0 : 10;
         }

         if( hStderr != FS_ERROR )
         {
            if( nErrBuf == nErrSize )
            {
               if( nErrSize == 0 )
                  nErrSize = HB_STD_BUFFER_SIZE;
               else
                  nErrSize += nErrSize >> 1;
               pErrBuf = ( char * ) hb_xrealloc( pErrBuf, nErrSize + 1 );
            }
            nLen = hb_fsReadLarge( hStderr, pErrBuf + nErrBuf, nErrSize - nErrBuf );
            if( nLen > 0 )
               nErrBuf += nLen;
            else if( fBlocked )
            {
               hb_fsClose( hStderr );
               hStderr = FS_ERROR;
               --iPipeCount;
            }
            if( dwWait )
               dwWait = nLen > 0 ? 0 : 10;
         }

         if( fFinished )
         {
            if( dwWait != 0 )
               break;
         }
         else if( hStdin != FS_ERROR )
         {
            nLen = ! fBlocked && nStdInLen > 4096 ? 4096 : nStdInLen;
            nLen = hb_fsWriteLarge( hStdin, pStdInBuf, nLen );
            pStdInBuf += nLen;
            nStdInLen -= nLen;
            if( nStdInLen == 0 || ( fBlocked && nLen == 0 ) )
            {
               hb_fsClose( hStdin );
               hStdin = FS_ERROR;
               --iPipeCount;
            }
            else if( dwWait )
               dwWait = nLen > 0 ? 0 : 10;
         }

         if( iPipeCount == 0 )
            dwWait = INFINITE;
         dwResult = WaitForSingleObject( ( HANDLE ) hb_fsGetOsHandle( hProcess ), dwWait );
         if( dwResult == WAIT_OBJECT_0 )
         {
            if( GetExitCodeProcess( ( HANDLE ) hb_fsGetOsHandle( hProcess ), &dwResult ) )
               iResult = ( int ) dwResult;
            else
               iResult = -2;
            fFinished = HB_TRUE;
         }
      }

      if( hStdin != FS_ERROR )
         hb_fsClose( hStdin );
      if( hStdout != FS_ERROR )
         hb_fsClose( hStdout );
      if( hStderr != FS_ERROR )
         hb_fsClose( hStderr );

      CloseHandle( ( HANDLE ) hb_fsGetOsHandle( hProcess ) );

#elif defined( HB_OS_OS2 ) || defined( HB_OS_WIN )

      HB_MAXINT nTimeOut = 0;
      int iPipeCount = 0;

      if( nStdInLen == 0 && hStdin != FS_ERROR )
      {
         hb_fsClose( hStdin );
         hStdin = FS_ERROR;
      }
      if( hStdout == hStderr )
         hStderr = FS_ERROR;

      if( hStdin != FS_ERROR )
         ++iPipeCount;
      if( hStdout != FS_ERROR )
         ++iPipeCount;
      if( hStderr != FS_ERROR )
         ++iPipeCount;

      while( iPipeCount > 0 )
      {
         HB_MAXINT nNextTOut = 10;
         HB_SIZE nLen;

         if( hStdin != FS_ERROR )
         {
            if( iPipeCount == 1 )
               nLen = hb_fsWriteLarge( hStdin, pStdInBuf, nStdInLen );
            else
               nLen = hb_fsPipeWrite( hStdin, pStdInBuf, nStdInLen, nTimeOut );
            if( nLen == ( HB_SIZE ) ( iPipeCount == 1 ? 0 : FS_ERROR ) )
               nStdInLen = 0;
            else if( nLen > 0 )
            {
               pStdInBuf += nLen;
               nStdInLen -= nLen;
               nNextTOut = 0;
            }
            if( nStdInLen == 0 )
            {
               hb_fsClose( hStdin );
               hStdin = FS_ERROR;
               --iPipeCount;
            }
         }

         if( hStdout != FS_ERROR )
         {
            if( nOutBuf == nOutSize )
            {
               if( nOutSize == 0 )
                  nOutSize = HB_STD_BUFFER_SIZE;
               else
                  nOutSize += nOutSize >> 1;
               pOutBuf = ( char * ) hb_xrealloc( pOutBuf, nOutSize + 1 );
            }
            if( iPipeCount == 1 )
               nLen = hb_fsReadLarge( hStdout, pOutBuf + nOutBuf, nOutSize - nOutBuf );
            else
               nLen = hb_fsPipeRead( hStdout, pOutBuf + nOutBuf, nOutSize - nOutBuf, nTimeOut );
            if( nLen == ( HB_SIZE ) ( iPipeCount == 1 ? 0 : FS_ERROR ) )
            {
               hb_fsClose( hStdout );
               hStdout = FS_ERROR;
               --iPipeCount;
            }
            else if( nLen > 0 )
            {
               nOutBuf += nLen;
               nNextTOut = 0;
            }
         }

         if( hStderr != FS_ERROR )
         {
            if( nErrBuf == nErrSize )
            {
               if( nErrSize == 0 )
                  nErrSize = HB_STD_BUFFER_SIZE;
               else
                  nErrSize += nErrSize >> 1;
               pErrBuf = ( char * ) hb_xrealloc( pErrBuf, nErrSize + 1 );
            }
            if( iPipeCount == 1 )
               nLen = hb_fsReadLarge( hStderr, pErrBuf + nErrBuf, nErrSize - nErrBuf );
            else
               nLen = hb_fsPipeRead( hStderr, pErrBuf + nErrBuf, nErrSize - nErrBuf, nTimeOut );
            if( nLen == ( HB_SIZE ) ( iPipeCount == 1 ? 0 : FS_ERROR ) )
            {
               hb_fsClose( hStderr );
               hStderr = FS_ERROR;
               --iPipeCount;
            }
            else if( nLen > 0 )
            {
               nErrBuf += nLen;
               nNextTOut = 0;
            }
         }

         nTimeOut = nNextTOut;
      }

      if( hStdin != FS_ERROR )
         hb_fsClose( hStdin );
      if( hStdout != FS_ERROR )
         hb_fsClose( hStdout );
      if( hStderr != FS_ERROR )
         hb_fsClose( hStderr );

      iResult = hb_fsProcessValue( hProcess, HB_TRUE );

#elif defined( HB_OS_UNIX ) && ! defined( HB_OS_SYMBIAN )

      fd_set rfds, wfds, *prfds, *pwfds;
      HB_FHANDLE fdMax;
      HB_SIZE ul;
      int n;

      if( nStdInLen == 0 && hStdin != FS_ERROR )
      {
         hb_fsClose( hStdin );
         hStdin = FS_ERROR;
      }
      if( hStdout == hStderr )
         hStderr = FS_ERROR;

      if( hStdin != FS_ERROR )
         hb_fsPipeUnblock( hStdin );
      if( hStdout != FS_ERROR )
         hb_fsPipeUnblock( hStdout );
      if( hStderr != FS_ERROR )
         hb_fsPipeUnblock( hStderr );

      for( ;; )
      {
         fdMax = 0;
         prfds = pwfds = NULL;
         if( hStdout != FS_ERROR || hStderr != FS_ERROR )
         {
            FD_ZERO( &rfds );
            if( hStdout != FS_ERROR )
            {
               FD_SET( hStdout, &rfds );
               if( hStdout > fdMax )
                  fdMax = hStdout;
            }
            if( hStderr != FS_ERROR )
            {
               FD_SET( hStderr, &rfds );
               if( hStderr > fdMax )
                  fdMax = hStderr;
            }
            prfds = &rfds;
         }
         if( hStdin != FS_ERROR )
         {
            FD_ZERO( &wfds );
            FD_SET( hStdin, &wfds );
            if( hStdin > fdMax )
               fdMax = hStdin;
            pwfds = &wfds;
         }
         if( prfds == NULL && pwfds == NULL )
            break;

         n = select( fdMax + 1, prfds, pwfds, NULL, NULL );
         if( n > 0 )
         {
            if( hStdout != FS_ERROR && FD_ISSET( hStdout, &rfds ) )
            {
               if( nOutBuf == nOutSize )
               {
                  if( nOutSize == 0 )
                     nOutSize = HB_STD_BUFFER_SIZE;
                  else
                     nOutSize += nOutSize >> 1;
                  pOutBuf = ( char * ) hb_xrealloc( pOutBuf, nOutSize + 1 );
               }
               ul = hb_fsReadLarge( hStdout, pOutBuf + nOutBuf, nOutSize - nOutBuf );
               if( ul == 0 )
               {
                  /* zero bytes read after positive Select()
                   * - writing process closed the pipe
                   */
                  hb_fsClose( hStdout );
                  hStdout = FS_ERROR;
               }
               else
                  nOutBuf += ul;
            }

            if( hStderr != FS_ERROR && FD_ISSET( hStderr, &rfds ) )
            {
               if( nErrBuf == nErrSize )
               {
                  if( nErrSize == 0 )
                     nErrSize = HB_STD_BUFFER_SIZE;
                  else
                     nErrSize += nErrSize >> 1;
                  pErrBuf = ( char * ) hb_xrealloc( pErrBuf, nErrSize + 1 );
               }
               ul = hb_fsReadLarge( hStderr, pErrBuf + nErrBuf, nErrSize - nErrBuf );
               if( ul == 0 )
               {
                  /* zero bytes read after positive Select()
                   * - writing process closed the pipe
                   */
                  hb_fsClose( hStderr );
                  hStderr = FS_ERROR;
               }
               else
                  nErrBuf += ul;
            }

            if( hStdin != FS_ERROR && FD_ISSET( hStdin, &wfds ) )
            {
               ul = hb_fsWriteLarge( hStdin, pStdInBuf, nStdInLen );
               pStdInBuf += ul;
               nStdInLen -= ul;
               if( nStdInLen == 0 )
               {
                  hb_fsClose( hStdin );
                  hStdin = FS_ERROR;
               }
            }
         }
         else
            break;
      }

      if( hStdin != FS_ERROR )
         hb_fsClose( hStdin );
      if( hStdout != FS_ERROR )
         hb_fsClose( hStdout );
      if( hStderr != FS_ERROR )
         hb_fsClose( hStderr );

      iResult = hb_fsProcessValue( hProcess, HB_TRUE );

#else

      int iTODO;

      HB_SYMBOL_UNUSED( nStdInLen );

#endif
      hb_vmLock();
   }
}
#endif

   if( phStdout )
   {
      *pStdOutPtr = pOutBuf;
      *pulStdOut = nOutBuf;
   }
   if( phStderr && phStdout != phStderr )
   {
      *pStdErrPtr = pErrBuf;
      *pulStdErr = nErrBuf;
   }

   return iResult;
}

/* temporary hack for still missing sysconf() and chroot() in Watcom 1.9 */
#if defined( HB_OS_LINUX ) && defined( __WATCOMC__ ) && \
    __WATCOMC__ <= 1290
_WCRTLINK long sysconf( int __name )
{
   int iTODO;

   switch( __name )
   {
      case _SC_OPEN_MAX:
         return 1024;
      case _SC_CLK_TCK:
         return 100;
      case /* _SC_PAGE_SIZE */ 30:
         return 4096;
   }
   return -1;
}

_WCRTLINK int chroot( const char * __path )
{
   int iTODO;

   HB_SYMBOL_UNUSED( __path );

   return -1;
}
#endif
