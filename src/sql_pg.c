/* OpenVAS Manager
 * $Id$
 * Description: Manager Manage library: the SQL library.
 *
 * Authors:
 * Matthew Mundell <matthew.mundell@greenbone.net>
 *
 * Copyright:
 * Copyright (C) 2014 Greenbone Networks GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2,
 * or, at your option, any later version as published by the Free
 * Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "sql.h"
#include "tracef.h"

#include <assert.h>
#include <endian.h>
#include <arpa/inet.h>
#include <glib.h>
#include <netinet/in.h>
#include <postgresql/libpq-fe.h>
#include <string.h>

#include <openvas/base/array.h>


/* Headers of sql.c symbols used only here. */

int
sql_x (char*, va_list args, sql_stmt_t**);


/* Types. */

struct sql_stmt
{
  gchar *sql;             ///< SQL statement.
  PGresult *result;       ///< Result set.
  int current_row;        ///< Row position in results.
  int executed;           ///< Whether statement has been executed.
  array_t *param_values;  ///< Parameter values.
  GArray *param_lengths;  ///< Parameter lengths (int's).
  GArray *param_formats;  ///< Parameter formats (int's).
};


/* Variables. */

/**
 * @brief Handle on the database.
 */
PGconn *conn = NULL;


/* Helpers. */

/**
 * @brief Get whether backend is SQLite3.
 *
 * @return 0.
 */
int
sql_is_sqlite3 ()
{
  return 0;
}

/**
 * @brief Get main schema name.
 *
 * @return Schema name.
 */
const char *
sql_schema ()
{
  return "public";
}

/**
 * @brief Setup a LIMIT argument.
 *
 * @param[in]  max  Max.
 *
 * @return Argument for LIMIT as a static string.
 */
const char *
sql_select_limit (int max)
{
  static char string[20];
  if (max < 0)
    return "ALL";
  if (snprintf (string, 19, "%i", max) < 0)
    {
      g_warning ("%s: snprintf failed\n", __FUNCTION__);
      abort ();
    }
  string[19] = '\0';
  return string;
}

/**
 * @brief Add param to statement.
 *
 * @param[in]  stmt          Statement.
 * @param[in]  param_value   Value.
 * @param[in]  param_size    Size (used for binary format).
 * @param[in]  param_format  0 text, 1 binary.
 */
static void
sql_stmt_param_add (sql_stmt_t *stmt, const char *param_value,
                    int param_size, int param_format)
{
  array_add (stmt->param_values, g_strdup (param_value));
  g_array_append_val (stmt->param_lengths, param_size);
  g_array_append_val (stmt->param_formats, param_format);
}

/**
 * @brief Init statement, preserving SQL.
 *
 * @param[in]  stmt  Statement.
 */
static void
sql_stmt_init (sql_stmt_t *stmt)
{
  memset (stmt, 0, sizeof (*stmt));
  stmt->param_values = make_array ();
  stmt->param_lengths = g_array_new (FALSE, TRUE, sizeof (int));
  stmt->param_formats = g_array_new (FALSE, TRUE, sizeof (int));
  stmt->current_row = -1;
}

/**
 * @brief Check whether the database is open.
 *
 * @return 1 if open, else 0.
 */
int
sql_is_open ()
{
  return conn ? 1 : 0;
}

/**
 * @brief Log a NOTICE message.
 *
 * @param[in]  arg      Dummy arg.
 * @param[in]  message  Arg.
 *
 * @return 0 success, -1 error.
 */
static void
log_notice (void *arg, const char *message)
{
  g_debug ("%s", message);
}

/**
 * @brief Open the database.
 *
 * @param[in]  database  Database, or NULL for default.
 *
 * @return 0 success, -1 error.
 */
int
sql_open (const char *database)
{
  // FIX database
  conn = PQconnectdb ("dbname = tasks");
  if (PQstatus (conn) != CONNECTION_OK)
    {
      g_warning ("%s: PQconnectdb failed: %s\n",
                 __FUNCTION__,
                 PQerrorMessage (conn));
      return -1;
    }

  PQsetNoticeProcessor (conn, log_notice, NULL);

  tracef ("%s:   db: %s\n", __FUNCTION__, PQdb (conn));
  tracef ("%s: user: %s\n", __FUNCTION__, PQuser (conn));
  tracef ("%s: port: %s\n", __FUNCTION__, PQport (conn));

  return 0;
}

/**
 * @brief Close the database.
 */
void
sql_close ()
{
  PQfinish (conn);
  conn = NULL;
}

/**
 * @brief Close the database in a forked process.
 */
void
sql_close_fork ()
{
  // FIX PQfinish?
  conn = NULL;
}

/**
 * @brief Return 0.
 *
 * @return 0.
 */
int
sql_changes ()
{
  /* TODO PQcmdTuples needs a PQresult.  Callers use for info only anyway. */
  return 0;
}

/**
 * @brief Get the ID of the last inserted row.
 */
resource_t
sql_last_insert_rowid ()
{
  return sql_int ("SELECT LASTVAL ();");
}

/**
 * @brief Perform an SQL statement, retrying if database is busy or locked.
 *
 * @param[out] resource  Last inserted resource.
 * @param[in]  sql       Format string for SQL statement.
 * @param[in]  ...       Arguments for format string.
 */
void
sqli (resource_t *resource, char* sql, ...)
{
  gchar *new_sql;
  sql_stmt_t* stmt;
  int sql_x_ret;
  va_list args;

  assert (sql && strlen (sql) && (sql[strlen (sql) - 1] != ';'));

  /* Append RETURNING clause to SQL. */

  new_sql = g_strdup_printf ("%s RETURNING id;", sql);

  /* Run statement, returning integer. */

  va_start (args, sql);
  sql_x_ret = sql_x (new_sql, args, &stmt);
  va_end (args);
  g_free (new_sql);
  switch (sql_x_ret)
    {
      case  0:
        break;
      case  1:
        sql_finalize (stmt);
        abort ();
        return;
      default:
        assert (0);
        /* Fall through. */
      case -1:
        sql_finalize (stmt);
        abort ();
        return;
    }
  if (resource)
    *resource = sql_column_int64 (stmt, 0);
  sql_finalize (stmt);
}

/**
 * @brief Prepare a statement.
 *
 * @param[in]  retry  Whether to keep retrying while database is busy or locked.
 * @param[in]  log    Whether to keep retrying while database is busy or locked.
 * @param[in]  sql    Format string for SQL statement.
 * @param[in]  args   Arguments for format string.
 * @param[out] stmt   Statement return.
 *
 * @return 0 success, 1 gave up, -1 error.
 */
int
sql_prepare_internal (int retry, int log, const char* sql, va_list args,
                      sql_stmt_t **stmt)
{
  assert (stmt);

  *stmt = (sql_stmt_t*) g_malloc (sizeof (sql_stmt_t));
  sql_stmt_init (*stmt);
  (*stmt)->sql = g_strdup_vprintf (sql, args);

  if (log)
    tracef ("   sql: %s\n", (*stmt)->sql);

  return 0;
}

/**
 * @brief Execute a prepared statement.
 *
 * @param[in]  retry  Whether to keep retrying while database is busy or locked.
 * @param[in]  stmt   Statement.
 *
 * @return 0 complete, 1 row available in results, -1 error, -2 gave up.
 */
int
sql_exec_internal (int retry, sql_stmt_t *stmt)
{
  PGresult *result;

  assert (stmt->sql);

  if (stmt->executed == 0)
    {
      // FIX retry?

      result = PQexecParams (conn,
                             stmt->sql,
                             stmt->param_values->len,
                             NULL,                 /* Default param types. */
                             (const char* const*) stmt->param_values->pdata,
                             (const int*) stmt->param_lengths->data,
                             (const int*) stmt->param_formats->data,
                             1);                   /* Results as binary. */
      if (PQresultStatus (result) != PGRES_TUPLES_OK
          && PQresultStatus (result) != PGRES_COMMAND_OK)
        {
          g_warning ("%s: PQexec failed: %s (%i)\n",
                     __FUNCTION__,
                     PQresultErrorMessage (result),
                     PQresultStatus (result));
          g_warning ("%s: SQL: %s\n", __FUNCTION__, stmt->sql);
#if 0
          // FIX ?
          PQclear (result);
          PQfinish (conn);
#endif
          return -1;
        }

      stmt->result = result;
      stmt->executed = 1;
    }

  if (stmt->current_row < (PQntuples (stmt->result) - 1))
    {
      stmt->current_row++;
      return 1;
    }

  return 0;
}


/* Transactions. */

/**
 * @brief Begin an exclusive transaction.
 */
void
sql_begin_exclusive ()
{
  sql ("BEGIN;");
  sql ("LOCK TABLE meta IN ACCESS EXCLUSIVE MODE;");
}

/**
 * @brief Begin an exclusive transaction, giving up on failure.
 *
 * @return 0 got lock, 1 gave up, -1 error.
 */
int
sql_begin_exclusive_giveup ()
{
  int ret;

  ret = sql_giveup ("BEGIN;");
  if (ret)
    return ret;
  ret = sql_giveup ("LOCK TABLE meta IN ACCESS EXCLUSIVE MODE;");
  if (ret)
    {
      sql ("ROLLBACK;");
      return ret;
    }
  return 0;
}

/**
 * @brief Begin an immediate transaction.
 */
void
sql_begin_immediate ()
{
  sql ("BEGIN;");
  /* TODO This is just an exclusive lock. */
  sql ("LOCK TABLE meta IN ACCESS EXCLUSIVE MODE;");
}


/* Iterators. */

/**
 * @brief Get whether a column is NULL.
 *
 * @param[in]  iterator  Iterator.
 * @param[in]  col       Column offset.
 *
 * @return 1 if NULL, else 0.
 */
int
iterator_null (iterator_t* iterator, int col)
{
  if (iterator->done) abort ();
  assert (iterator->stmt->result);
  return PQgetisnull (iterator->stmt->result, 0, col);
}

/**
 * @brief Get a column name from an iterator.
 *
 * @param[in]  iterator  Iterator.
 * @param[in]  col       Column offset.
 *
 * @return Name of given column.
 */
const char*
iterator_column_name (iterator_t* iterator, int col)
{
  if (iterator->done) abort ();
  assert (iterator->stmt->result);
  return PQfname (iterator->stmt->result, col);
}

/**
 * @brief Get number of columns from an iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Number of columns.
 */
int
iterator_column_count (iterator_t* iterator)
{
  if (iterator->done) abort ();
  assert (iterator->stmt->result);
  return PQnfields (iterator->stmt->result);
}


/* Prepared statements. */

/**
 * @brief Bind a param to a statement.
 *
 * @param[in]  stmt          Statement.
 * @param[in]  position      Position in statement.
 * @param[in]  param_value   Param value.
 * @param[in]  param_size    Param size.
 * @param[in]  param_format  0 text, 1 binary.
 */
static
void
bind_param (sql_stmt_t *stmt, int position, const void *param_value,
            int param_size, int param_format)
{
  if (position > stmt->param_values->len + 1)
    {
      g_critical ("%s: binding out of order: parameter %i after %i",
                  __FUNCTION__,
                  position,
                  stmt->param_values->len);
      abort ();
    }
  sql_stmt_param_add (stmt, param_value, param_size, param_format);
}

/**
 * @brief Bind a blob to a statement.
 *
 * @param[in]  stmt        Statement.
 * @param[in]  position    Position in statement.
 * @param[in]  value       Blob.
 * @param[in]  value_size  Blob size.
 *
 * @return 0 success, -1 error.
 */
int
sql_bind_blob (sql_stmt_t *stmt, int position, const void *value,
               int value_size)
{
  bind_param (stmt, position, value, value_size, 1);
  return 0;
}

/**
 * @brief Bind an int64 value to a statement.
 *
 * @param[in]  stmt        Statement.
 * @param[in]  position    Position in statement.
 * @param[in]  value       Value.
 *
 * @return 0 success, -1 error.
 */
int
sql_bind_int64 (sql_stmt_t *stmt, int position, long long int value)
{
  abort (); // FIX needs to send in address of value
  bind_param (stmt, position, 0 /* value */, sizeof (long long int), 1);
  return 0;
}

/**
 * @brief Bind a double value to a statement.
 *
 * @param[in]  stmt        Statement.
 * @param[in]  position    Position in statement.
 * @param[in]  value       Value.
 *
 * @return 0 success, -1 error.
 */
int
sql_bind_double (sql_stmt_t *stmt, int position, double value)
{
  abort (); // FIX needs to send in address of value
  bind_param (stmt, position, 0 /* value */, sizeof (double), 1);
  return 0;
}

/**
 * @brief Bind a text value to a statement.
 *
 * @param[in]  stmt        Statement.
 * @param[in]  position    Position in statement.
 * @param[in]  value       Value.
 * @param[in]  value_size  Value size, or -1 to use strlen of value.
 *
 * @return 0 success, -1 error.
 */
int
sql_bind_text (sql_stmt_t *stmt, int position, const gchar *value,
               gsize value_size)
{
  bind_param (stmt,
              position,
              value,
              value_size == -1 ? strlen (value) : value_size,
              0);
  return 0;
}

/**
 * @brief Free a prepared statement.
 *
 * @param[in]  stmt  Statement.
 */
void
sql_finalize (sql_stmt_t *stmt)
{
  PQclear (stmt->result);
  g_free (stmt->sql);
  array_free (stmt->param_values);
  g_array_free (stmt->param_lengths, TRUE);
  g_array_free (stmt->param_formats, TRUE);
  g_free (stmt);
}

/**
 * @brief Reset a prepared statement.
 *
 * @param[in]  stmt  Statement.
 *
 * @return 0 success, -1 error.
 */
int
sql_reset (sql_stmt_t *stmt)
{
  gchar *sql;

  PQclear (stmt->result);
  array_free (stmt->param_values);
  g_array_free (stmt->param_lengths, TRUE);
  g_array_free (stmt->param_formats, TRUE);

  sql = stmt->sql;
  sql_stmt_init (stmt);
  stmt->sql = sql;
  return 0;
}

/**
 * @brief Return a column as a double from a prepared statement.
 *
 * It's up to the caller to ensure that there is a row available.
 *
 * @param[in]  stmt      Statement.
 * @param[in]  position  Column position.
 *
 * @return 0 success, -1 error.
 */
double
sql_column_double (sql_stmt_t *stmt, int position)
{
  if (PQgetisnull (stmt->result, stmt->current_row, position))
    return 0.0;
  return *((double*) PQgetvalue (stmt->result, stmt->current_row, position));
}

/**
 * @brief Return a column as text from a prepared statement.
 *
 * It's up to the caller to ensure that there is a row available.
 *
 * @param[in]  stmt      Statement.
 * @param[in]  position  Column position.
 *
 * @return 0 success, -1 error.
 */
const char *
sql_column_text (sql_stmt_t *stmt, int position)
{
  if (PQgetisnull (stmt->result, stmt->current_row, position))
    return NULL;
  return (const char*) PQgetvalue (stmt->result, stmt->current_row, position);
}

/**
 * @brief Return a column as an integer from a prepared statement.
 *
 * It's up to the caller to ensure that there is a row available.
 *
 * @param[in]  stmt      Statement.
 * @param[in]  position  Column position.
 *
 * @return 0 success, -1 error.
 */
int
sql_column_int (sql_stmt_t *stmt, int position)
{
  char *cell;

  if (PQgetisnull (stmt->result, stmt->current_row, position))
    return 0;

  cell = PQgetvalue (stmt->result, stmt->current_row, position);

  switch (PQftype (stmt->result, position))
    {
      case 21:  /* INT2OID */
        return (int) ntohs (*((uint16_t *) cell));

      default:
        /* Hope for the best. */
      case 23:  /* INT4OID */
        return (int) ntohl (*((uint32_t *) cell));

      case 20:  /* INT8OID */
        /* be64 is network byte order. */
        return (int) be64toh (*((uint64_t *) cell));
    }
}

/**
 * @brief Return a column as an int64 from a prepared statement.
 *
 * It's up to the caller to ensure that there is a row available.
 *
 * @param[in]  stmt      Statement.
 * @param[in]  position  Column position.
 *
 * @return 0 success, -1 error.
 */
long long int
sql_column_int64 (sql_stmt_t *stmt, int position)
{
  char *cell;

  if (PQgetisnull (stmt->result, stmt->current_row, position))
    return 0;

  cell = PQgetvalue (stmt->result, stmt->current_row, position);

  switch (PQftype (stmt->result, position))
    {
      case 21:  /* INT2OID */
        return (long long int) ntohs (*((uint16_t *) cell));

      case 23:  /* INT4OID */
        return (long long int) ntohl (*((uint32_t *) cell));

      default:
        /* Hope for the best. */
      case 20:  /* INT8OID */
        /* be64 is network byte order. */
        return (long long int) be64toh (*((uint64_t *) cell));
    }
}
