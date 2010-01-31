/* Copyright 2004-2008 MySQL AB

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

#include "MYODBCUtil.h"


/*!
  \brief    Creates/replaces the given data source.

            The use of SQLWriteDSNToIni() means this function
            does not really update an existing DSN so much as
            replaces it.
*/
BOOL MYODBCUtilWriteDataSource( MYODBCUTIL_DATASOURCE *pDataSource )
{
    SAVE_MODE();

    /*
        SQLWriteDSNToIni is *supposed* to replace any existing DSN
        with same name but fails (at least on unixODBC) to do so.
        So we ensure that any existing DSN with same name is removed
        with the following call.
    */
    if ( !SQLRemoveDSNFromIni( pDataSource->pszDSN ) )
        return FALSE;

    RESTORE_MODE();

    /* create/replace data source name */
    if ( !SQLWriteDSNToIni( pDataSource->pszDSN, pDataSource->pszDRIVER ) )
        return FALSE;

    RESTORE_MODE();

    /* A little helper to avoid duplicated code. */
#define WRITE_VALUE(field, name) \
    if (pDataSource->psz##field && \
        !SQLWritePrivateProfileString(pDataSource->pszDSN, name, \
                                      pDataSource->psz##field, "odbc.ini")) \
      return FALSE; \
    RESTORE_MODE()

    /* add details */
    WRITE_VALUE(DATABASE, "DATABASE");
    WRITE_VALUE(DESCRIPTION, "DESCRIPTION");
    WRITE_VALUE(OPTION, "OPTION");
    WRITE_VALUE(PASSWORD, "PWD");
    WRITE_VALUE(PORT, "PORT");
    WRITE_VALUE(SERVER, "SERVER");
    WRITE_VALUE(SOCKET, "SOCKET");
    WRITE_VALUE(STMT, "STMT");
    WRITE_VALUE(USER, "UID");
    WRITE_VALUE(SSLCA, "SSLCA");
    WRITE_VALUE(SSLCAPATH, "SSLCAPATH");
    WRITE_VALUE(SSLCERT, "SSLCERT");
    WRITE_VALUE(SSLCIPHER, "SSLCIPHER");
    WRITE_VALUE(SSLKEY, "SSLKEY");
    WRITE_VALUE(SSLVERIFY, "SSLVERIFY");
    WRITE_VALUE(CHARSET, "CHARSET");

    return TRUE;
}
