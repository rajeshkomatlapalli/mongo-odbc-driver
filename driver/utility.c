/*
  Copyright (C) 1995-2007 MySQL AB

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.

  There are special exceptions to the terms and conditions of the GPL
  as it is applied to this software. View the full text of the exception
  in file LICENSE.exceptions in the top-level directory of this software
  distribution.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/**
  @file  utility.c
  @brief Utility functions
*/

#include "driver.h"
#include "errmsg.h"
#include <ctype.h>


/**
  Execute a SQL statement.

  @param[in] dbc   The database connection
  @param[in] query The query to execute
*/
SQLRETURN odbc_stmt(DBC FAR *dbc, const char *query)
{
    SQLRETURN result= SQL_SUCCESS;

    pthread_mutex_lock(&dbc->lock);
    if ( check_if_server_is_alive(dbc) ||
         mysql_real_query(&dbc->mysql,query,strlen(query)) )
    {
        result= set_conn_error(dbc,MYERR_S1000,mysql_error(&dbc->mysql),
                               mysql_errno(&dbc->mysql));
    }
    pthread_mutex_unlock(&dbc->lock);
    return result;
}


/**
  Link a list of fields to the current statement result.

  @todo This is a terrible idea. We need to purge this.

  @param[in] stmt        The statement to modify
  @param[in] fields      The fields to attach to the statement
  @param[in] field_count The number of fields
*/
void mysql_link_fields(STMT *stmt, MYSQL_FIELD *fields, uint field_count)
{
    MYSQL_RES *result;
    pthread_mutex_lock(&stmt->dbc->lock);
    result= stmt->result;
    result->fields= fields;
    result->field_count= field_count;
    result->current_field= 0;
    fix_result_types(stmt);
    pthread_mutex_unlock(&stmt->dbc->lock);
}


/**
  Figure out the ODBC result types for each column in the result set.

  @param[in] stmt The statement with result types to be fixed.
*/
void fix_result_types(STMT *stmt)
{
    uint i;
    MYSQL_RES *result= stmt->result;

    stmt->state= ST_EXECUTED;  /* Mark set found */
    if ( (stmt->odbc_types= (SQLSMALLINT*)
          my_malloc(sizeof(SQLSMALLINT)*result->field_count, MYF(0))) )
    {
        for ( i= 0 ; i < result->field_count ; i++ )
        {
            MYSQL_FIELD *field= result->fields+i;
            stmt->odbc_types[i]= (SQLSMALLINT) unireg_to_c_datatype(field);
        }
    }
    /*
      Fix default values for bound columns
      Normally there isn't any bound columns at this stage !
    */
    if ( stmt->bind )
    {
        if ( stmt->bound_columns < result->field_count )
        {
            if ( !(stmt->bind= (BIND*) my_realloc((char*) stmt->bind,
                                                  sizeof(BIND) * result->field_count,
                                                  MYF(MY_FREE_ON_ERROR))) )
            {
                /* We should in principle give an error here */
                stmt->bound_columns= 0;
                return;
            }
            bzero((stmt->bind+stmt->bound_columns),
                  (result->field_count -stmt->bound_columns)*sizeof(BIND));
            stmt->bound_columns= result->field_count;
        }
        /* Fix default types and pointers to fields */

        mysql_field_seek(result,0);
        for ( i= 0; i < result->field_count ; i++ )
        {
            if ( stmt->bind[i].fCType == SQL_C_DEFAULT )
                stmt->bind[i].fCType= stmt->odbc_types[i];
            stmt->bind[i].field= mysql_fetch_field(result);
        }
    }
}


/**
  Change a string with a length to a NUL-terminated string.

  @param[in,out] to      A buffer to write the string into, which must be at
                         at least length + 1 bytes long.
  @param[in]     from    A pointer to the beginning of the source string.
  @param[in]     length  The length of the string, or SQL_NTS if it is
                         already NUL-terminated.

  @return A pointer to a NUL-terminated string.
*/
char *fix_str(char *to, const char *from, int length)
{
    if ( !from )
        return "";
    if ( length == SQL_NTS )
        return (char *)from;
    strmake(to,from,length);
    return to;
}


/*
  @type    : myodbc internal
  @purpose : duplicate the string
*/

char *dupp_str(char *from,int length)
{
    char *to;
    if ( !from )
        return my_strdup("",MYF(MY_WME));
    if ( length == SQL_NTS )
        length= strlen(from);
    if ( (to= my_malloc(length+1,MYF(MY_WME))) )
    {
        memcpy(to,from,length);
        to[length]= 0;
    }
    return to;
}


/*
  @type    : myodbc internal
  @purpose : copies the string data to rgbValue buffer. If rgbValue
  is NULL, then returns warning with full length, else
  copies the cbValueMax length from 'src' and returns it.
*/

SQLRETURN copy_str_data(SQLSMALLINT HandleType, SQLHANDLE Handle,
                        SQLCHAR FAR *rgbValue,
                        SQLSMALLINT cbValueMax,
                        SQLSMALLINT FAR *pcbValue,char FAR *src)
{
    SQLSMALLINT dummy;

    if ( !pcbValue )
        pcbValue= &dummy;

    if ( cbValueMax == SQL_NTS )
        cbValueMax= *pcbValue= strlen(src);

    else if ( cbValueMax < 0 )
        return set_handle_error(HandleType,Handle,MYERR_S1090,NULL,0);
    else
    {
        cbValueMax= cbValueMax ? cbValueMax - 1 : 0;
        *pcbValue= strlen(src);
    }

    if ( rgbValue )
        strmake((char*) rgbValue, src, cbValueMax);

    if ( min(*pcbValue , cbValueMax) != *pcbValue )
        return SQL_SUCCESS_WITH_INFO;
    return SQL_SUCCESS;
}


/*
  Copy a field to a byte string.

  @param[in]     stmt         Pointer to statement
  @param[out]    result       Buffer for result
  @param[in]     result_bytes Size of result buffer (in bytes)
  @param[out]    avail_bytes  Pointer to buffer for storing number of bytes
                              available as result
  @param[in]     field        Field being stored
  @param[in]     src          Source data for result
  @param[in]     src_bytes    Length of source data (in bytes)

  @return Standard ODBC result code
*/
SQLRETURN
copy_binary_result(STMT *stmt,
                   SQLCHAR *result, SQLLEN result_bytes, SQLLEN *avail_bytes,
                   MYSQL_FIELD *field __attribute__((unused)),
                   char *src, unsigned long src_bytes)
{
  SQLRETURN rc= SQL_SUCCESS;
  ulong copy_bytes;

  if (!result_bytes)
    result= 0;       /* Don't copy anything! */

  /* Apply max length to source data, if one was specified. */
  if (stmt->stmt_options.max_length &&
      src_bytes > stmt->stmt_options.max_length)
    src_bytes= stmt->stmt_options.max_length;

  /* Initialize the source offset */
  if (!stmt->getdata.source)
    stmt->getdata.source= src;
  else
  {
    src_bytes-= stmt->getdata.source - src;
    src= stmt->getdata.source;

    /* If we've already retrieved everything, return SQL_NO_DATA_FOUND */
    if (src_bytes == 0)
      return SQL_NO_DATA_FOUND;
  }

  copy_bytes= min((unsigned long)result_bytes, src_bytes);

  if (result)
    memcpy(result, src, copy_bytes);

  if (avail_bytes)
    *avail_bytes= src_bytes;

  stmt->getdata.source+= copy_bytes;

  if (src_bytes > (unsigned long)result_bytes)
  {
    set_stmt_error(stmt, "01004", NULL, 0);
    rc= SQL_SUCCESS_WITH_INFO;
  }

  return rc;
}


/*
  Copy a field to an ANSI result string.

  @param[in]     stmt         Pointer to statement
  @param[out]    result       Buffer for result
  @param[in]     result_bytes Size of result buffer (in bytes)
  @param[out]    avail_bytes  Pointer to buffer for storing number of bytes
                              available as result
  @param[in]     field        Field being stored
  @param[in]     src          Source data for result
  @param[in]     src_bytes    Length of source data (in bytes)

  @return Standard ODBC result code
*/
SQLRETURN
copy_ansi_result(STMT *stmt,
                 SQLCHAR *result, SQLLEN result_bytes, SQLLEN *avail_bytes,
                 MYSQL_FIELD *field, char *src, unsigned long src_bytes)
{
  SQLRETURN rc= SQL_SUCCESS;
  char *src_end;
  SQLCHAR *result_end;
  ulong used_bytes= 0, used_chars= 0, error_count= 0;
  CHARSET_INFO *to_cs= stmt->dbc->ansi_charset_info,
               *from_cs= get_charset(field->charsetnr ? field->charsetnr :
                                     UTF8_CHARSET_NUMBER,
                                     MYF(0));

  if (!result_bytes)
    result= 0;       /* Don't copy anything! */

  /*
   If we don't have to do any charset conversion, we can just use
   copy_binary_result() and NUL-terminate the buffer here.
  */
  if (to_cs->number == from_cs->number)
  {
    SQLLEN bytes;
    if (!avail_bytes)
      avail_bytes= &bytes;

    if (result_bytes)
      result_bytes--;

    rc= copy_binary_result(stmt, result, result_bytes, avail_bytes,
                           field, src, src_bytes);

    if (SQL_SUCCEEDED(rc) && result)
      result[min(*avail_bytes, result_bytes)]= '\0';

    return rc;
  }

  result_end= result + result_bytes - 1;
  /*
    Handle when result_bytes is 1 -- we have room for the NUL termination,
    but nothing else.
  */
  if (result == result_end)
  {
    *result= '\0';
    result= 0;
  }

  /* Apply max length to source data, if one was specified. */
  if (stmt->stmt_options.max_length &&
      src_bytes > stmt->stmt_options.max_length)
    src_bytes= stmt->stmt_options.max_length;
  src_end= src + src_bytes;

  /* Initialize the source offset */
  if (!stmt->getdata.source)
    stmt->getdata.source= src;
  else
    src= stmt->getdata.source;

  /* If we've already retrieved everything, return SQL_NO_DATA_FOUND */
  if (stmt->getdata.dst_bytes != (ulong)~0L &&
      stmt->getdata.dst_offset >= stmt->getdata.dst_bytes)
    return SQL_NO_DATA_FOUND;

  /*
    If we have leftover bytes from an earlier character conversion,
    copy as much as we can into place.
  */
  if (stmt->getdata.latest_bytes)
  {
    int new_bytes= min(stmt->getdata.latest_bytes - stmt->getdata.latest_used,
                       result_end - result);
    memcpy(result, stmt->getdata.latest + stmt->getdata.latest_used, new_bytes);
    if (new_bytes + stmt->getdata.latest_used == stmt->getdata.latest_bytes)
      stmt->getdata.latest_bytes= 0;

    result+= new_bytes;

    if (result == result_end)
    {
      *result= '\0';
      result= NULL;
    }

    used_bytes+= new_bytes;
    stmt->getdata.latest_used+= new_bytes;
  }

  while (src < src_end)
  {
    /* Find the conversion functions. */
    int (*mb_wc)(struct charset_info_st *, my_wc_t *, const uchar *,
                 const uchar *) = from_cs->cset->mb_wc;
    int (*wc_mb)(struct charset_info_st *, my_wc_t, uchar *s,
                 uchar *e)= to_cs->cset->wc_mb;
    my_wc_t wc;
    uchar dummy[7]; /* Longer than any single character in our charsets. */
    int to_cnvres;

    int cnvres= (*mb_wc)(from_cs, &wc, (uchar *)src, (uchar *)src_end);
    if (cnvres == MY_CS_ILSEQ)
    {
      error_count++;
      cnvres= 1;
      wc= '?';
    }
    else if (cnvres < 0 && cnvres > MY_CS_TOOSMALL)
    {
      error_count++;
      cnvres= abs(cnvres);
      wc= '?';
    }
    else if (cnvres < 0)
      return set_stmt_error(stmt, "HY000",
                            "Unknown failure when converting character "
                            "from server character set.", 0);

convert_to_out:
    /*
     We always convert into a temporary buffer, so we can properly handle
     characters that are going to get split across requests.
    */
    to_cnvres= (*wc_mb)(to_cs, wc, result ? result : dummy,
                        (result ? result_end : dummy + sizeof(dummy)));

    if (to_cnvres > 0)
    {
      used_chars+= 1;
      used_bytes+= to_cnvres;

      if (result)
        result+= to_cnvres;

      src+= cnvres;

      if (result && result == result_end)
      {
        if (stmt->getdata.dst_bytes != (ulong)~0L)
        {
          stmt->getdata.source+= cnvres;
          break;
        }
        *result= '\0';
        result= NULL;
      }
      else if (!result)
        continue;

      stmt->getdata.source+= cnvres;
    }
    else if (result && to_cnvres <= MY_CS_TOOSMALL)
    {
      /*
       If we didn't have enough room for the character, we convert into
       stmt->getdata.latest and copy what we can. The next call to
       SQLGetData() will then copy what it can to the next buffer.
      */
      stmt->getdata.latest_bytes= (*wc_mb)(to_cs, wc, stmt->getdata.latest,
                                           stmt->getdata.latest +
                                           sizeof(stmt->getdata.latest));

      stmt->getdata.latest_used= min(stmt->getdata.latest_bytes,
                                     result_end - result);
      memcpy(result, stmt->getdata.latest, stmt->getdata.latest_used);
      result+= stmt->getdata.latest_used;
      *result= '\0';
      result= NULL;

      used_chars+= 1;
      used_bytes+= stmt->getdata.latest_bytes;

      src+= stmt->getdata.latest_bytes;
      stmt->getdata.source+= stmt->getdata.latest_bytes;
    }
    else if (stmt->getdata.latest_bytes == MY_CS_ILUNI && wc != '?')
    {
      error_count++;
      wc= '?';
      goto convert_to_out;
    }
    else
      return set_stmt_error(stmt, "HY000",
                            "Unknown failure when converting character "
                            "to result character set.", 0);
  }

  if (result)
    *result= 0;

  if (stmt->getdata.dst_bytes == (ulong)~0L)
  {
    stmt->getdata.dst_bytes= used_bytes;
    stmt->getdata.dst_offset= 0;
  }

  if (avail_bytes)
    *avail_bytes= stmt->getdata.dst_bytes - stmt->getdata.dst_offset;

  stmt->getdata.dst_offset+= min((ulong)(result_bytes ? result_bytes - 1 : 0),
                                 used_bytes);

  /* Did we truncate the data? */
  if (stmt->getdata.dst_bytes > stmt->getdata.dst_offset)
  {
    set_stmt_error(stmt, "01004", NULL, 0);
    rc= SQL_SUCCESS_WITH_INFO;
  }

  /* Did we encounter any character conversion problems? */
  if (error_count)
  {
    set_stmt_error(stmt, "22018", NULL, 0);
    rc= SQL_SUCCESS_WITH_INFO;
  }

  return rc;
}


/**
  Copy a result from the server into a buffer as a SQL_C_WCHAR.

  @param[in]     stmt        Pointer to statement
  @param[out]    result      Buffer for result
  @param[in]     avail_bytes Size of result buffer (in characters)
  @param[out]    used_len    Pointer to buffer for storing amount of buffer used
  @param[in]     field       Field being stored
  @param[in]     src         Source data for result
  @param[in]     src_bytes   Length of source data (in bytes)

  @return Standard ODBC result code
*/
SQLRETURN
copy_wchar_result(STMT *stmt,
                  SQLWCHAR *result, SQLINTEGER result_len, SQLLEN *avail_bytes,
                  MYSQL_FIELD *field, char *src, long src_bytes)
{
  SQLRETURN rc= SQL_SUCCESS;
  char *src_end;
  SQLWCHAR *result_end;
  ulong used_chars= 0, error_count= 0;
  CHARSET_INFO *from_cs= get_charset(field->charsetnr ? field->charsetnr :
                                     UTF8_CHARSET_NUMBER,
                                     MYF(0));

  if (!result_len)
    result= NULL; /* Don't copy anything! */

  result_end= result + result_len - 1;

  if (result == result_end)
  {
    *result= 0;
    result= 0;
  }

  /* Apply max length to source data, if one was specified. */
  if (stmt->stmt_options.max_length &&
      (ulong)src_bytes > stmt->stmt_options.max_length)
    src_bytes= stmt->stmt_options.max_length;
  src_end= src + src_bytes;

  /* Initialize the source data */
  if (!stmt->getdata.source)
    stmt->getdata.source= src;
  else
    src= stmt->getdata.source;

  /* If we've already retrieved everything, return SQL_NO_DATA_FOUND */
  if (stmt->getdata.dst_bytes != (ulong)~0L &&
      stmt->getdata.dst_offset >= stmt->getdata.dst_bytes)
    return SQL_NO_DATA_FOUND;

  /* We may have a leftover char from the last call. */
  if (stmt->getdata.latest_bytes)
  {
    memcpy(result, stmt->getdata.latest, sizeof(SQLWCHAR));
    result++;

    if (result == result_end)
    {
      *result= 0;
      result= NULL;
    }

    used_chars+= 1;
    stmt->getdata.latest_bytes= 0;
  }


  while (src < src_end)
  {
    /* Find the conversion functions. */
    int (*mb_wc)(struct charset_info_st *, my_wc_t *, const uchar *,
                 const uchar *) = from_cs->cset->mb_wc;
    int (*wc_mb)(struct charset_info_st *, my_wc_t, uchar *s,
                 uchar *e)= utf8_charset_info->cset->wc_mb;
    my_wc_t wc;
    uchar u8[5]; /* Max length of utf-8 string we'll see. */
    SQLWCHAR dummy[2]; /* If SQLWCHAR is UTF-16, we may need two chars. */
    int to_cnvres;

    int cnvres= (*mb_wc)(from_cs, &wc, (uchar *)src, (uchar *)src_end);
    if (cnvres == MY_CS_ILSEQ)
    {
      error_count++;
      cnvres= 1;
      wc= '?';
    }
    else if (cnvres < 0 && cnvres > MY_CS_TOOSMALL)
    {
      error_count++;
      cnvres= abs(cnvres);
      wc= '?';
    }
    else if (cnvres < 0)
      return set_stmt_error(stmt, "HY000",
                            "Unknown failure when converting character "
                            "from server character set.", 0);

convert_to_out:
    /*
     We always convert into a temporary buffer, so we can properly handle
     characters that are going to get split across requests.
    */
    to_cnvres= (*wc_mb)(utf8_charset_info, wc, u8, u8 + sizeof(u8));

    if (to_cnvres > 0)
    {
      u8[to_cnvres]= '\0';

      src+= cnvres;

      if (sizeof(SQLWCHAR) == 4)
      {
        utf8toutf32(u8, (UTF32 *)(result ? result : dummy));
        if (result)
          result++;
        used_chars+= 1;

        if (result && result == result_end)
        {
          if (stmt->getdata.dst_bytes != (ulong)~0L)
          {
            stmt->getdata.source+= cnvres;
            break;
          }
          *result= 0;
          result= NULL;
        }
      }
      else
      {
        UTF32 u32;
        UTF16 out[2];
        int chars;
        utf8toutf32(u8, &u32);
        chars= utf32toutf16(u32, (UTF16 *)out);

        if (result)
          *result++= out[0];

        used_chars+= chars;

        if (chars > 1 && result && result != result_end)
          *result++= out[1];
        else if (chars > 1 && result)
        {
          *((SQLWCHAR *)stmt->getdata.latest)= out[1];
          stmt->getdata.latest_bytes= 2;
          stmt->getdata.latest_used= 0;
          *result= 0;
          result= NULL;

          if (stmt->getdata.dst_bytes != (ulong)~0L)
          {
            stmt->getdata.source+= cnvres;
            break;
          }
        }
        else if (chars > 1)
          continue;
      }

      stmt->getdata.source+= cnvres;
    }
    else if (stmt->getdata.latest_bytes == MY_CS_ILUNI && wc != '?')
    {
      error_count++;
      wc= '?';
      goto convert_to_out;
    }
    else
      return set_stmt_error(stmt, "HY000",
                            "Unknown failure when converting character "
                            "to result character set.", 0);
  }

  if (result)
    *result= 0;

  if (stmt->getdata.dst_bytes == (ulong)~0L)
  {
    stmt->getdata.dst_bytes= used_chars * sizeof(SQLWCHAR);
    stmt->getdata.dst_offset= 0;
  }

  if (avail_bytes)
    *avail_bytes= stmt->getdata.dst_bytes - stmt->getdata.dst_offset;

  stmt->getdata.dst_offset+= min((ulong)(result_len ? result_len - 1 : 0),
                                 used_chars) * sizeof(SQLWCHAR);

  /* Did we truncate the data? */
  if (stmt->getdata.dst_bytes > stmt->getdata.dst_offset)
  {
    set_stmt_error(stmt, "01004", NULL, 0);
    rc= SQL_SUCCESS_WITH_INFO;
  }

  /* Did we encounter any character conversion problems? */
  if (error_count)
  {
    set_stmt_error(stmt, "22018", NULL, 0);
    rc= SQL_SUCCESS_WITH_INFO;
  }

  return rc;
}


/*
  @type    : myodbc internal
  @purpose : is used when converting a binary string to a SQL_C_CHAR
*/

SQLRETURN copy_binhex_result(STMT *stmt,
                             SQLCHAR *rgbValue, SQLINTEGER cbValueMax,
                             SQLLEN *pcbValue,
                             MYSQL_FIELD *field __attribute__((unused)),
                             char *src, ulong src_length)
{
  /** @todo padding of BINARY */
    char *dst= (char*) rgbValue;
    ulong length;
    ulong max_length= stmt->stmt_options.max_length;
    ulong *offset= &stmt->getdata.src_offset;
#if MYSQL_VERSION_ID >= 40100
    char NEAR _dig_vec[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
#endif

    if ( !cbValueMax )
        dst= 0;  /* Don't copy anything! */
    if ( max_length ) /* If limit on char lengths */
    {
        set_if_smaller(cbValueMax,(long) max_length+1);
        set_if_smaller(src_length,(max_length+1)/2);
    }
    if ( *offset == (ulong) ~0L )
        *offset= 0;   /* First call */
    else if ( *offset >= src_length )
        return SQL_NO_DATA_FOUND;
    src+= *offset;
    src_length-= *offset;
    length= cbValueMax ? (ulong)(cbValueMax-1)/2 : 0;
    length= min(src_length,length);
    (*offset)+= length;     /* Fix for next call */
    if ( pcbValue )
        *pcbValue= src_length*2;
    if ( dst )  /* Bind allows null pointers */
    {
        ulong i;
        for ( i= 0 ; i < length ; i++ )
        {
            *dst++= _dig_vec[(uchar) *src >> 4];
            *dst++= _dig_vec[(uchar) *src++ & 15];
        }
        *dst= 0;
    }
    if ( (ulong) cbValueMax > length*2 )
        return SQL_SUCCESS;

    set_stmt_error(stmt, "01004", NULL, 0);
    return SQL_SUCCESS_WITH_INFO;
}


/**
  Get the SQL data type and (optionally) type name for a MYSQL_FIELD.

  @param[in]  stmt
  @param[in]  field
  @param[out] buff

  @return  The SQL data type.
*/
SQLSMALLINT get_sql_data_type(STMT *stmt, MYSQL_FIELD *field, char *buff)
{
  my_bool field_is_binary= test(field->charsetnr == 63);

  switch (field->type) {
  case MYSQL_TYPE_BIT:
    if (buff)
      (void)strmov(buff, "bit");
    /*
      MySQL's BIT type can have more than one bit, in which case we treat
      it as a BINARY field.
    */
    return (field->length > 1) ? SQL_BINARY : SQL_BIT;

  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
    if (buff)
      (void)strmov(buff, "decimal");
    return SQL_DECIMAL;

  case MYSQL_TYPE_TINY:
    /* MYSQL_TYPE_TINY could either be a TINYINT or a single CHAR. */
    if (buff)
    {
      buff= strmov(buff, (field->flags & NUM_FLAG) ? "tinyint" : "char");
      if (field->flags & UNSIGNED_FLAG)
        (void)strmov(buff, " unsigned");
    }
    return (field->flags & NUM_FLAG) ? SQL_TINYINT : SQL_CHAR;

  case MYSQL_TYPE_SHORT:
    if (buff)
    {
      buff= strmov(buff, "smallint");
      if (field->flags & UNSIGNED_FLAG)
        (void)strmov(buff, " unsigned");
    }
    return SQL_SMALLINT;

  case MYSQL_TYPE_INT24:
    if (buff)
    {
      buff= strmov(buff, "mediumint");
      if (field->flags & UNSIGNED_FLAG)
        (void)strmov(buff, " unsigned");
    }
    return SQL_INTEGER;

  case MYSQL_TYPE_LONG:
    if (buff)
    {
      buff= strmov(buff, "integer");
      if (field->flags & UNSIGNED_FLAG)
        (void)strmov(buff, " unsigned");
    }
    return SQL_INTEGER;

  case MYSQL_TYPE_LONGLONG:
    if (buff)
    {
      buff= strmov(buff, "bigint");
      if (field->flags & UNSIGNED_FLAG)
        (void)strmov(buff, " unsigned");
    }

    if (stmt->dbc->flag & FLAG_NO_BIGINT)
      return SQL_INTEGER;

    return SQL_BIGINT;

  case MYSQL_TYPE_FLOAT:
    if (buff)
    {
      buff= strmov(buff, "float");
      if (field->flags & UNSIGNED_FLAG)
        (void)strmov(buff, " unsigned");
    }
    return SQL_REAL;

  case MYSQL_TYPE_DOUBLE:
    if (buff)
    {
      buff= strmov(buff, "double");
      if (field->flags & UNSIGNED_FLAG)
        (void)strmov(buff, " unsigned");
    }
    return SQL_DOUBLE;

  case MYSQL_TYPE_NULL:
    if (buff)
      (void)strmov(buff, "null");
    return SQL_VARCHAR;

  case MYSQL_TYPE_YEAR:
    if (buff)
      (void)strmov(buff, "year");
    return SQL_SMALLINT;

  case MYSQL_TYPE_TIMESTAMP:
    if (buff)
      (void)strmov(buff, "timestamp");
    if (stmt->dbc->env->odbc_ver == SQL_OV_ODBC3)
      return SQL_TYPE_TIMESTAMP;
    return SQL_TIMESTAMP;

  case MYSQL_TYPE_DATETIME:
    if (buff)
      (void)strmov(buff, "datetime");
    if (stmt->dbc->env->odbc_ver == SQL_OV_ODBC3)
      return SQL_TYPE_TIMESTAMP;
    return SQL_TIMESTAMP;

  case MYSQL_TYPE_NEWDATE:
  case MYSQL_TYPE_DATE:
    if (buff)
      (void)strmov(buff, "date");
    if (stmt->dbc->env->odbc_ver == SQL_OV_ODBC3)
      return SQL_TYPE_DATE;
    return SQL_DATE;

  case MYSQL_TYPE_TIME:
    if (buff)
      (void)strmov(buff, "time");
    if (stmt->dbc->env->odbc_ver == SQL_OV_ODBC3)
      return SQL_TYPE_TIME;
    return SQL_TIME;

  case MYSQL_TYPE_STRING:
    if (buff)
      (void)strmov(buff, field_is_binary ? "binary" : "char");

    return field_is_binary ? SQL_BINARY : SQL_CHAR;

  /*
    MYSQL_TYPE_VARCHAR is never actually sent, this just silences
    a compiler warning.
  */
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_VAR_STRING:
#ifdef SERVER_BUG_10491_FIXED
    /**
      @todo Re-enable this when Bug #10491 is fixed in the server.
    */
    if (buff)
      (void)strmov(buff, field_is_binary ? "varbinary" : "varchar");

    return field_is_binary ? SQL_VARBINARY : SQL_VARCHAR;
#else
    if (buff)
      (void)strmov(buff, "varchar");
    return SQL_VARCHAR;
#endif

  case MYSQL_TYPE_TINY_BLOB:
    if (buff)
      (void)strmov(buff, field_is_binary ? "tinyblob" : "tinytext");

    return field_is_binary ? SQL_LONGVARBINARY : SQL_LONGVARCHAR;

  case MYSQL_TYPE_BLOB:
    if (buff)
      (void)strmov(buff, field_is_binary ? "blob" : "text");

    return field_is_binary ? SQL_LONGVARBINARY : SQL_LONGVARCHAR;

  case MYSQL_TYPE_MEDIUM_BLOB:
    if (buff)
      (void)strmov(buff, field_is_binary ? "mediumblob" : "mediumtext");

    return field_is_binary ? SQL_LONGVARBINARY : SQL_LONGVARCHAR;

  case MYSQL_TYPE_LONG_BLOB:
    if (buff)
      (void)strmov(buff, field_is_binary ? "longblob" : "longtext");

    return field_is_binary ? SQL_LONGVARBINARY : SQL_LONGVARCHAR;

  case MYSQL_TYPE_ENUM:
    if (buff)
      (void)strmov(buff, "enum");
    return SQL_CHAR;

  case MYSQL_TYPE_SET:
    if (buff)
      (void)strmov(buff, "set");
    return SQL_CHAR;

  case MYSQL_TYPE_GEOMETRY:
    if (buff)
      (void)strmov(buff, "geometry");
    return SQL_LONGVARBINARY;
  }

  if (buff)
    *buff= '\0';
  return SQL_UNKNOWN_TYPE;
}


/**
  Get the column size (in characters) of a field, as defined at:
    http://msdn2.microsoft.com/en-us/library/ms711786.aspx

  @param[in]  stmt
  @param[in]  field
  @param[in]  actual  If true, field->max_length is used instead of
                      field->length, to retrieve the actual length of
                      data in the field

  @return  The column size of the field
*/
SQLLEN get_column_size(STMT *stmt __attribute__((unused)), MYSQL_FIELD *field,
                       my_bool actual)
{
  SQLLEN length= actual ? field->max_length : field->length;

  switch (field->type) {
  case MYSQL_TYPE_TINY:
    return (field->flags & NUM_FLAG) ? 3 : 1;

  case MYSQL_TYPE_SHORT:
    return 5;

  case MYSQL_TYPE_LONG:
    return 10;

  case MYSQL_TYPE_FLOAT:
    return 7;

  case MYSQL_TYPE_DOUBLE:
    return 15;

  case MYSQL_TYPE_NULL:
    return 0;

  case MYSQL_TYPE_LONGLONG:
    return (field->flags & UNSIGNED_FLAG) ? 20 : 19;

  case MYSQL_TYPE_INT24:
    return 8;

  case MYSQL_TYPE_DATE:
    return 10;

  case MYSQL_TYPE_TIME:
    return 8;

  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_NEWDATE:
    return 19;

  case MYSQL_TYPE_YEAR:
    return 4;

  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
    return (length -
            test(!(field->flags & UNSIGNED_FLAG)) - /* sign? */
            test(field->decimals));                 /* decimal point? */

  case MYSQL_TYPE_BIT:
    /*
      We treat a BIT(n) as a SQL_BIT if n == 1, otherwise we treat it
      as a SQL_BINARY, so length is (bits + 7) / 8.
    */
    if (length == 1)
      return 1;
    return (length + 7) / 8;

  case MYSQL_TYPE_ENUM:
  case MYSQL_TYPE_SET:
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_GEOMETRY:
    if (field->charsetnr == 63)
      return length;
    else
      return length;
  }

  return SQL_NO_TOTAL;
}


/**
  Get the decimal digits of a field, as defined at:
    http://msdn2.microsoft.com/en-us/library/ms709314.aspx

  @param[in]  stmt
  @param[in]  field

  @return  The decimal digits, or @c SQL_NO_TOTAL where it makes no sense
*/
SQLLEN get_decimal_digits(STMT *stmt __attribute__((unused)),
                          MYSQL_FIELD *field)
{
  switch (field->type) {
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
    return field->decimals;

  /* All exact numeric types. */
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_YEAR:
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_DATETIME:
    return 0;

  /* We treat MYSQL_TYPE_BIT as an exact numeric type only for BIT(1). */
  case MYSQL_TYPE_BIT:
    if (field->length == 1)
      return 0;

  default:
    return SQL_NO_TOTAL;
  }
}


/**
  Get the transfer octet length of a field, as defined at:
    http://msdn2.microsoft.com/en-us/library/ms713979.aspx

  @param[in]  stmt
  @param[in]  field

  @return  The transfer octet length
*/
SQLLEN get_transfer_octet_length(STMT *stmt __attribute__((unused)),
                                 MYSQL_FIELD *field)
{
  SQLLEN length= field->length;

  switch (field->type) {
  case MYSQL_TYPE_TINY:
    return 1;

  case MYSQL_TYPE_SHORT:
    return 2;

  case MYSQL_TYPE_INT24:
    return 3;

  case MYSQL_TYPE_LONG:
    return 4;

  case MYSQL_TYPE_FLOAT:
    return 4;

  case MYSQL_TYPE_DOUBLE:
    return 8;

  case MYSQL_TYPE_NULL:
    return 1;

  case MYSQL_TYPE_LONGLONG:
    return 20;

  case MYSQL_TYPE_DATE:
    return sizeof(SQL_DATE_STRUCT);

  case MYSQL_TYPE_TIME:
    return sizeof(SQL_TIME_STRUCT);

  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_NEWDATE:
    return sizeof(SQL_TIMESTAMP_STRUCT);

  case MYSQL_TYPE_YEAR:
    return 1;

  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
    return (field->length -
            test(!(field->flags & UNSIGNED_FLAG)) - /* sign? */
            test(field->decimals));                 /* decimal point? */

  case MYSQL_TYPE_BIT:
    /*
      We treat a BIT(n) as a SQL_BIT if n == 1, otherwise we treat it
      as a SQL_BINARY, so length is (bits + 7) / 8. field->length has
      the number of bits.
    */
    return (field->length + 7) / 8;

  case MYSQL_TYPE_STRING:
    if (stmt->dbc->flag & FLAG_PAD_SPACE)
      length= field->max_length;
    /* FALLTHROUGH */

  case MYSQL_TYPE_ENUM:
  case MYSQL_TYPE_SET:
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_GEOMETRY:
    if (field->charsetnr == 63)
      return length;
    if (field->charsetnr != stmt->dbc->ansi_charset_info->number)
      return length * stmt->dbc->ansi_charset_info->mbmaxlen;
    return length;
  }

  return SQL_NO_TOTAL;
}


/**
  Get the display size of a field, as defined at:
    http://msdn2.microsoft.com/en-us/library/ms713974.aspx

  @param[in]  stmt
  @param[in]  field

  @return  The display size
*/
SQLLEN get_display_size(STMT *stmt __attribute__((unused)),MYSQL_FIELD *field)
{
  CHARSET_INFO *charset= get_charset(field->charsetnr, MYF(0));
  unsigned int mbmaxlen= charset ? charset->mbmaxlen : 1;

  switch (field->type) {
  case MYSQL_TYPE_TINY:
    return 3 + test(field->flags & UNSIGNED_FLAG);

  case MYSQL_TYPE_SHORT:
    return 5 + test(field->flags & UNSIGNED_FLAG);

  case MYSQL_TYPE_INT24:
    return 8 + test(field->flags & UNSIGNED_FLAG);

  case MYSQL_TYPE_LONG:
    return 10 + test(field->flags & UNSIGNED_FLAG);

  case MYSQL_TYPE_FLOAT:
    return 14;

  case MYSQL_TYPE_DOUBLE:
    return 24;

  case MYSQL_TYPE_NULL:
    return 1;

  case MYSQL_TYPE_LONGLONG:
    return 20;

  case MYSQL_TYPE_DATE:
    return 10;

  case MYSQL_TYPE_TIME:
    return 8;

  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_NEWDATE:
    return 19;

  case MYSQL_TYPE_YEAR:
    return 4;

  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
    return field->length;

  case MYSQL_TYPE_BIT:
    /*
      We treat a BIT(n) as a SQL_BIT if n == 1, otherwise we treat it
      as a SQL_BINARY, so display length is (bits + 7) / 8 * 2.
      field->length has the number of bits.
    */
    if (field->length == 1)
      return 1;
    return (field->length + 7) / 8 * 2;

  case MYSQL_TYPE_ENUM:
  case MYSQL_TYPE_SET:
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_GEOMETRY:
    if (field->charsetnr == 63)
      return field->length * 2;
    else
      return field->length / mbmaxlen;
  }

  return SQL_NO_TOTAL;
}


/*
  @type    : myodbc internal
  @purpose : returns internal type to C type
*/

int unireg_to_c_datatype(MYSQL_FIELD *field)
{
    switch ( field->type )
    {
        case MYSQL_TYPE_LONGLONG: /* Must be returned as char */
        default:
            return SQL_C_CHAR;
        case MYSQL_TYPE_BIT:
            /*
              MySQL's BIT type can have more than one bit, in which case we
              treat it as a BINARY field.
            */
            return (field->length > 1) ? SQL_C_BINARY : SQL_C_BIT;
        case MYSQL_TYPE_TINY:
            return SQL_C_TINYINT;
        case MYSQL_TYPE_YEAR:
        case MYSQL_TYPE_SHORT:
            return SQL_C_SHORT;
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
            return SQL_C_LONG;
        case MYSQL_TYPE_FLOAT:
            return SQL_C_FLOAT;
        case MYSQL_TYPE_DOUBLE:
            return SQL_C_DOUBLE;
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_DATETIME:
            return SQL_C_TIMESTAMP;
        case MYSQL_TYPE_NEWDATE:
        case MYSQL_TYPE_DATE:
            return SQL_C_DATE;
        case MYSQL_TYPE_TIME:
            return SQL_C_TIME;
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
            return SQL_C_BINARY;
    }
}


/*
  @type    : myodbc internal
  @purpose : returns default C type for a given SQL type
*/

int default_c_type(int sql_data_type)
{
    switch ( sql_data_type )
    {
        case SQL_CHAR:
        case SQL_VARCHAR:
        case SQL_LONGVARCHAR:
        case SQL_DECIMAL:
        case SQL_NUMERIC:
        default:
            return SQL_C_CHAR;
        case SQL_BIGINT:
            return SQL_C_SBIGINT;
        case SQL_BIT:
            return SQL_C_BIT;
        case SQL_TINYINT:
            return SQL_C_TINYINT;
        case SQL_SMALLINT:
            return SQL_C_SHORT;
        case SQL_INTEGER:
            return SQL_C_LONG;
        case SQL_REAL:
        case SQL_FLOAT:
            return SQL_C_FLOAT;
        case SQL_DOUBLE:
            return SQL_C_DOUBLE;
        case SQL_BINARY:
        case SQL_VARBINARY:
        case SQL_LONGVARBINARY:
            return SQL_C_BINARY;
        case SQL_DATE:
        case SQL_TYPE_DATE:
            return SQL_C_DATE;
        case SQL_TIME:
        case SQL_TYPE_TIME:
            return SQL_C_TIME;
        case SQL_TIMESTAMP:
        case SQL_TYPE_TIMESTAMP:
            return SQL_C_TIMESTAMP;
    }
}


/*
  @type    : myodbc internal
  @purpose : returns bind length
*/

ulong bind_length(int sql_data_type,ulong length)
{
    switch ( sql_data_type )
    {
        default:                  /* For CHAR, VARCHAR, BLOB...*/
            return length;
        case SQL_C_BIT:
        case SQL_C_TINYINT:
        case SQL_C_STINYINT:
        case SQL_C_UTINYINT:
            return 1;
        case SQL_C_SHORT:
        case SQL_C_SSHORT:
        case SQL_C_USHORT:
            return 2;
        case SQL_C_LONG:
        case SQL_C_SLONG:
        case SQL_C_ULONG:
            return sizeof(SQLINTEGER);
        case SQL_C_FLOAT:
            return sizeof(float);
        case SQL_C_DOUBLE:
            return sizeof(double);
        case SQL_C_DATE:
        case SQL_C_TYPE_DATE:
            return sizeof(DATE_STRUCT);
        case SQL_C_TIME:
        case SQL_C_TYPE_TIME:
            return sizeof(TIME_STRUCT);
        case SQL_C_TIMESTAMP:
        case SQL_C_TYPE_TIMESTAMP:
            return sizeof(TIMESTAMP_STRUCT);
        case SQL_C_SBIGINT:
        case SQL_C_UBIGINT:
            return sizeof(longlong);
    }
}

/*
  @type    : myodbc internal
  @purpose : convert a possible string to a timestamp value
*/

my_bool str_to_ts(SQL_TIMESTAMP_STRUCT *ts, const char *str, int zeroToMin)
{ 
    uint year, length;
    char buff[15],*to;
    SQL_TIMESTAMP_STRUCT tmp_timestamp;

    if ( !ts )
        ts= (SQL_TIMESTAMP_STRUCT *) &tmp_timestamp;

    for ( to= buff ; *str && to < buff+sizeof(buff)-1 ; str++ )
    {
        if ( isdigit(*str) )
            *to++= *str;
    }

    length= (uint) (to-buff);

    if ( length == 6 || length == 12 )  /* YYMMDD or YYMMDDHHMMSS */
    {
        memmove(to+2, to, length);
        if ( buff[0] <= '6' )
        {
            buff[0]='2';
            buff[1]='0';
        }
        else
        {
            buff[0]='1';
            buff[1]='9';
        }
        length+= 2;
        to+= 2;
    }

    if ( length < 14 )
        strfill(to,14 - length,'0');
    else
        *to= 0;

    year= (digit(buff[0])*1000+digit(buff[1])*100+digit(buff[2])*10+digit(buff[3]));

    if (!strncmp(&buff[4], "00", 2) || !strncmp(&buff[6], "00", 2))
    {
      if (!zeroToMin) /* Don't convert invalid */
        return 1;

      /* convert invalid to min allowed */
      if (!strncmp(&buff[4], "00", 2))
        buff[5]= '1';
      if (!strncmp(&buff[6], "00", 2))
        buff[7]= '1';
    }

    ts->year=   year;
    ts->month=  digit(buff[4])*10+digit(buff[5]);
    ts->day=    digit(buff[6])*10+digit(buff[7]);
    ts->hour=   digit(buff[8])*10+digit(buff[9]);
    ts->minute= digit(buff[10])*10+digit(buff[11]);
    ts->second= digit(buff[12])*10+digit(buff[13]);
    ts->fraction= 0;
    return 0;
}

/*
  @type    : myodbc internal
  @purpose : convert a possible string to a time value
*/

my_bool str_to_time_st(SQL_TIME_STRUCT *ts, const char *str)
{ 
    char buff[12],*to;
    SQL_TIME_STRUCT tmp_time;

    if ( !ts )
        ts= (SQL_TIME_STRUCT *) &tmp_time;

    for ( to= buff ; *str && to < buff+sizeof(buff)-1 ; str++ )
    {
        if ( isdigit(*str) )
            *to++= *str;
    }

    ts->hour=   digit(buff[0])*10+digit(buff[1]);
    ts->minute= digit(buff[2])*10+digit(buff[3]);
    ts->second= digit(buff[4])*10+digit(buff[5]);
    return 0;
}

/*
  @type    : myodbc internal
  @purpose : convert a possible string to a data value. if
             zeroToMin is specified, YEAR-00-00 dates will be
             converted to the min valid ODBC date
*/

my_bool str_to_date(SQL_DATE_STRUCT *rgbValue, const char *str,
                    uint length, int zeroToMin)
{
    uint field_length,year_length,digits,i,date[3];
    const char *pos;
    const char *end= str+length;
    for ( ; !isdigit(*str) && str != end ; str++ ) ;
    /*
      Calculate first number of digits.
      If length= 4, 8 or >= 14 then year is of format YYYY
      (YYYY-MM-DD,  YYYYMMDD)
    */
    for ( pos= str; pos != end && isdigit(*pos) ; pos++ ) ;
    digits= (uint) (pos-str);
    year_length= (digits == 4 || digits == 8 || digits >= 14) ? 4 : 2;
    field_length= year_length-1;

    for ( i= 0 ; i < 3 && str != end; i++ )
    {
        uint tmp_value= (uint) (uchar) (*str++ - '0');
        while ( str != end && isdigit(str[0]) && field_length-- )
        {
            tmp_value= tmp_value*10 + (uint) (uchar) (*str - '0');
            str++;
        }
        date[i]= tmp_value;
        while ( str != end && !isdigit(*str) )
            str++;
        field_length= 1;   /* Rest fields can only be 2 */
    }
    if (i <= 1 || (i > 1 && !date[1]) || (i > 2 && !date[2]))
    {
      if (!zeroToMin) /* Convert? */
        return 1;

      rgbValue->year=  date[0];
      rgbValue->month= (i > 1 && date[1]) ? date[1] : 1;
      rgbValue->day=   (i > 2 && date[2]) ? date[2] : 1;
    }
    else
    {
      while ( i < 3 )
        date[i++]= 1;

      rgbValue->year=  date[0];
      rgbValue->month= date[1];
      rgbValue->day=   date[2];
    }
    return 0;
}


/*
  @type    : myodbc internal
  @purpose : convert a time string to a (ulong) value.
  At least following formats are recogniced
  HHMMSS HHMM HH HH.MM.SS  {t HH:MM:SS }
  @return  : HHMMSS
*/

ulong str_to_time_as_long(const char *str,uint length)
{
    uint i,date[3];
    const char *end= str+length;

    if ( length == 0 )
        return 0;

    for ( ; !isdigit(*str) && str != end ; str++ ) length--;

    for ( i= 0 ; i < 3 && str != end; i++ )
    {
        uint tmp_value= (uint) (uchar) (*str++ - '0');
        length--;

        while ( str != end && isdigit(str[0]) )
        {
            tmp_value= tmp_value*10 + (uint) (uchar) (*str - '0');
            str++; 
            length--;
        }
        date[i]= tmp_value;
        while ( str != end && !isdigit(*str) )
        {
            str++;
            length--;
        }
    }
    if ( length && str != end )
        return str_to_time_as_long(str, length);/* timestamp format */

    if ( date[0] > 10000L || i < 3 )    /* Properly handle HHMMSS format */
        return(ulong) date[0];

    return(ulong) date[0] * 10000L + (ulong) (date[1]*100L+date[2]);
}


/*
  @type    : myodbc internal
  @purpose : if there was a long time since last question, check that
  the server is up with mysql_ping (to force a reconnect)
*/

int check_if_server_is_alive( DBC FAR *dbc )
{
    time_t seconds= (time_t) time( (time_t*)0 );
    int result= 0;

    if ( (ulong)(seconds - dbc->last_query_time) >= CHECK_IF_ALIVE )
    {
        if ( mysql_ping( &dbc->mysql ) )
        {
            /*  BUG: 14639

                A. The 4.1 doc says when mysql_ping() fails we can get one
                of the following errors from mysql_errno();

                    CR_COMMANDS_OUT_OF_SYNC
                    CR_SERVER_GONE_ERROR
                    CR_UNKNOWN_ERROR   

                But if you do a mysql_ping() after bringing down the server
                you get CR_SERVER_LOST.

                PAH - 9.MAR.06
            */
            
            if ( mysql_errno( &dbc->mysql ) == CR_SERVER_LOST )
                result = 1;
        }
    }
    dbc->last_query_time = seconds;

    return result;
}


/*
  @type    : myodbc3 internal
  @purpose : appends quoted string to dynamic string
*/

my_bool dynstr_append_quoted_name(DYNAMIC_STRING *str, const char *name)
{
    uint tmp= strlen(name);
    char *pos;
    if ( dynstr_realloc(str,tmp+3) )
        return 1;
    pos= str->str+str->length;
    *pos='`';
    memcpy(pos+1,name,tmp);
    pos[tmp+1]='`';
    pos[tmp+2]= 0;        /* Safety */
    str->length+= tmp+2;
    return 0;
}


/*
  @type    : myodbc3 internal
  @purpose : reset the db name to current_database()
*/

my_bool reget_current_catalog(DBC FAR *dbc)
{
    my_free(dbc->database,MYF(0));
    if ( odbc_stmt(dbc, "select database()") )
    {
        return 1;
    }
    else
    {
        MYSQL_RES *res;
        MYSQL_ROW row;

        if ( (res= mysql_store_result(&dbc->mysql)) &&
             (row= mysql_fetch_row(res)) )
        {
/*            if (cmp_database(row[0], dbc->database)) */
            {
                if ( row[0] )
                    dbc->database = my_strdup(row[0], MYF(MY_WME));
                else
                    dbc->database = strdup( "null" );
            }
        }
        mysql_free_result(res);
    }

    return 0;
}


/*
  @type    : myodbc internal
  @purpose : compare strings without regarding to case
*/

int myodbc_strcasecmp(const char *s, const char *t)
{
  while (toupper((uchar) *s) == toupper((uchar) *t++))
    if (!*s++)
      return 0;
  return((int) toupper((uchar) s[0]) - (int) toupper((uchar) t[-1]));
}


/*
  @type    : myodbc internal
  @purpose : compare strings without regarding to case
*/

int myodbc_casecmp(const char *s, const char *t, uint len)
{
  while (len-- != 0 && toupper(*s++) == toupper(*t++))
    ;
  return (int)len + 1;
}


/*
  @type    : myodbc3 internal
  @purpose : logs the queries sent to server
*/

void query_print(FILE *log_file,char *query)
{
    if ( log_file && query )
        fprintf(log_file, "%s;\n",query);
}


FILE *init_query_log(void)
{
    FILE *query_log;

    if ( (query_log= fopen(DRIVER_QUERY_LOGFILE, "w")) )
    {
        fprintf(query_log,"-- Query logging\n");
        fprintf(query_log,"--\n");
        fprintf(query_log,"--  Driver name: %s  Version: %s\n",DRIVER_NAME,
                DRIVER_VERSION);
#ifdef HAVE_LOCALTIME_R
        {
            time_t now= time(NULL);
            struct tm start;
            localtime_r(&now,&start);

            fprintf(query_log,"-- Timestamp: %02d%02d%02d %2d:%02d:%02d\n",
                    start.tm_year % 100,
                    start.tm_mon+1,
                    start.tm_mday,
                    start.tm_hour,
                    start.tm_min,
                    start.tm_sec);
#endif /* HAVE_LOCALTIME_R */
            fprintf(query_log,"\n");
        }
    }
    return query_log;
}


void end_query_log(FILE *query_log)
{
    if ( query_log )
    {
        fclose(query_log);
        query_log= 0;
    }
}


my_bool is_minimum_version(const char *server_version,const char *version,
                           uint length)
{
    if ( strncmp(server_version,version,length) >= 0 )
        return TRUE;
    return FALSE;
}


/**
 Escapes a string that may contain wildcard characters (%, _) and other
 problematic characters (", ', \n, etc). Like mysql_real_escape_string() but
 also including % and _.

 @param[in]   mysql         Pointer to MYSQL structure
 @param[out]  to            Buffer for escaped string
 @param[in]   to_length     Length of destination buffer, or 0 for "big enough"
 @param[in]   from          The string to escape
 @param[in]   length        The length of the string to escape

*/
ulong myodbc_escape_wildcard(MYSQL *mysql __attribute__((unused)),
                             char *to, ulong to_length,
                             const char *from, ulong length)
{
  const char *to_start= to;
  const char *end, *to_end=to_start + (to_length ? to_length-1 : 2*length);
  my_bool overflow= FALSE;
  CHARSET_INFO *charset_info= mysql->charset;
  my_bool use_mb_flag= use_mb(charset_info);
  for (end= from + length; from < end; from++)
  {
    char escape= 0;
    int tmp_length;
    if (use_mb_flag && (tmp_length= my_ismbchar(charset_info, from, end)))
    {
      if (to + tmp_length > to_end)
      {
        overflow= TRUE;
        break;
      }
      while (tmp_length--)
	*to++= *from++;
      from--;
      continue;
    }
    /*
     If the next character appears to begin a multi-byte character, we
     escape that first byte of that apparent multi-byte character. (The
     character just looks like a multi-byte character -- if it were actually
     a multi-byte character, it would have been passed through in the test
     above.)

     Without this check, we can create a problem by converting an invalid
     multi-byte character into a valid one. For example, 0xbf27 is not
     a valid GBK character, but 0xbf5c is. (0x27 = ', 0x5c = \)
    */
    if (use_mb_flag && (tmp_length= my_mbcharlen(charset_info, *from)) > 1)
      escape= *from;
    else
    switch (*from) {
    case 0:				/* Must be escaped for 'mysql' */
      escape= '0';
      break;
    case '\n':				/* Must be escaped for logs */
      escape= 'n';
      break;
    case '\r':
      escape= 'r';
      break;
    case '\\':
      escape= '\\';
      break;
    case '\'':
      escape= '\'';
      break;
    case '"':				/* Better safe than sorry */
      escape= '"';
      break;
    case '_':
      escape= '_';
      break;
    case '%':
      escape= '%';
      break;
    case '\032':			/* This gives problems on Win32 */
      escape= 'Z';
      break;
    }
    if (escape)
    {
      if (to + 2 > to_end)
      {
        overflow= TRUE;
        break;
      }
      *to++= '\\';
      *to++= escape;
    }
    else
    {
      if (to + 1 > to_end)
      {
        overflow= TRUE;
        break;
      }
      *to++= *from;
    }
  }
  *to= 0;
  return overflow ? (ulong)~0 : (ulong) (to - to_start);
}


