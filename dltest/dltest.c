/* Copyright 2006-2007 MySQL AB

   The MySQL Connector/ODBC is licensed under the terms of the
   GPL, like most MySQL Connectors. There are special exceptions
   to the terms and conditions of the GPL as it is applied to
   this software, see the FLOSS License Exception available on
   mysql.com.

   This program is free software; you can redistribute it and/or modify
   it under the terms of version 2 of the GNU General Public License as
   published by the Free Software Foundation.

   There are special exceptions to the terms and conditions of the GPL as it
   is applied to this software. View the full text of the exception in file
   EXCEPTIONS in the directory of this software distribution.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <stdio.h>
#include <stdlib.h>

#ifdef WIN32
    #include <windows.h>
#else
    #include <ltdl.h>
#endif

char *szSyntax =
"\n" \
"**********************************************\n" \
"* dltest                                     *\n" \
"**********************************************\n" \
"* Syntax                                     *\n" \
"*                                            *\n" \
"*      dltest libName Symbol                 *\n" \
"*      dltest lib:libName... sym:Symbol...   *\n" \
"*                                            *\n" \
"* libName                                    *\n" \
"*                                            *\n" \
"*      Full path + file name of share to test*\n" \
"*                                            *\n" \
"* Symbol                                     *\n" \
"*                                            *\n" \
"*      ie a function name in the share       *\n" \
"*                                            *\n" \
"* Notes                                      *\n" \
"*                                            *\n" \
"*      This can be placed into a makefile    *\n" \
"*      to throw an error if test fails.      *\n" \
"*                                            *\n" \
"*      If this segfaults you probably have an*\n" \
"*      unresolved symbol in the lib. This is *\n" \
"*      not caught since dltest started using *\n" \
"*      libtool. Linux users can refer to the *\n" \
"*      man page for dlopen to create a       *\n" \
"*      better test.                          *\n" \
"*                                            *\n" \
"*                                            *\n" \
"* Examples                                   *\n" \
"*                                            *\n" \
"*      dltest /usr/lib/libMy.so MyFunc       *\n" \
"*                                            *\n" \
"**********************************************\n\n";

#ifdef WIN32
typedef HMODULE DLTestModule;
#else
typedef void *  DLTestModule;
#endif

static void dltest_dlinit(void);
static DLTestModule dltest_dlopen(const char *);
static void dltest_dlsym(DLTestModule, const char *);
static void dltest_dlclose(DLTestModule);


int main( int argc, char *argv[] )
{
  DLTestModule hModule = NULL;

  if ( argc < 2 )
  {
      printf( szSyntax );
      exit( 1 );
  }

  /* At least one argument, a library path */

  dltest_dlinit();

  if ( strncmp(argv[1],"lib:",4) == 0 || strncmp(argv[1],"sym:",4) == 0 )
  {
    /* Alternative API, can handle multiple libs and symbols, in any mix */
    int i;
    for (i = 1; i < argc; i++)
    {
      if (strncmp(argv[1],"lib:",4) == 0)
      {
        hModule = dltest_dlopen(argv[i]+4);     /* Open a new module */
      }
      else if (strncmp(argv[1],"sym:",4) == 0)
      {
        dltest_dlsym(hModule,argv[i]+4);
      }
    }

    /*
      Why close at all, and if we load libraries dependent on each
      other, closing will prevent a later opened lib from accessing
      symbols from the previous one, at least on AIX 5.2
    */
  }
  else
  {
    /* Old API */
    hModule = dltest_dlopen(argv[1]);
    if ( argc > 2 )
      dltest_dlsym(hModule,argv[2]);
    dltest_dlclose(hModule);
  }

  return(0);
}


static void dltest_dlinit(void)
{
#ifndef WIN32
  if ( lt_dlinit() )
  {
    printf( "[%s][%d] ERROR: Failed to lt_dlinit()\n", __FILE__, __LINE__ );
    exit( 1 );
  }
#endif
}

static DLTestModule dltest_dlopen(const char *path)
{
#ifdef WIN32
  DLTestModule hModule = LoadLibrary((LPCSTR)path);
  if ( !hModule )
  {
    LPVOID pszMsg;
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                  NULL,
                  GetLastError(),
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                  (LPTSTR) &pszMsg,
                  0, 
                  NULL);
    printf("[%s][%d] ERROR LoadLibrary(): %s\n", __FILE__, __LINE__, pszMsg);
    LocalFree(pszMsg);
    exit(1);
  }
#else
  DLTestModule hModule = lt_dlopen(path);
  if ( !hModule )
  {
    printf("[%s][%d] ERROR dlopen(): %s\n", __FILE__, __LINE__, lt_dlerror());
    exit(1);
  }
#endif /* WIN32 */

  printf("[%s][%d] SUCCESS: Loaded %s\n", __FILE__, __LINE__, path);
  return hModule;
}


static void dltest_dlclose(DLTestModule hModule)
{
#ifdef WIN32
  FreeLibrary(hModule);
#else
  lt_dlclose(hModule);
#endif /* WIN32 */
}


static void dltest_dlsym(DLTestModule hModule, const char *sym)
{
  void (*pFunc)();
#ifdef WIN32
  pFunc = (void (*)())GetProcAddress(hModule,sym);
  if ( !pFunc )
  {
    LPVOID pszMsg;
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                  NULL,
                  GetLastError(),
                  MAKELANGID(LANG_NEUTRAL,SUBLANG_DEFAULT),
                  (LPTSTR) &pszMsg,
                  0, 
                  NULL);
    printf("[%s][%d] ERROR: Could not find %s. %s\n",__FILE__,__LINE__,sym,pszMsg);
    LocalFree(pszMsg);
    FreeLibrary(hModule);
    exit(1);
  }
#else
  pFunc = (void (*)())lt_dlsym(hModule,sym);
/* PAH - lt_dlerror() is not a good indicator of success    */
/*		if ( (pError = lt_dlerror()) != NULL )              */
  if ( !pFunc )
  {
    const char *pError;
    if ( (pError = lt_dlerror()) != NULL )
      printf("[%s][%d] ERROR: %s\n Could not find %s\n",__FILE__,__LINE__,pError,sym);
    else
      printf("[%s][%d] ERROR: Could not find %s\n",__FILE__,__LINE__,sym);
    exit(1);
  }
#endif
  printf("[%s][%d] SUCCESS: Found %s\n",__FILE__,__LINE__,sym);
}
