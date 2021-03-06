/*----------------------------------------------------------------------------
 *
 *     varchar2.c
 *     VARCHAR2 type for PostgreSQL.
 *
 *----------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/hash.h"
#include "libpq/pqformat.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "mb/pg_wchar.h"
#include "fmgr.h"

#include "oraschema.h"

static VarChar *varchar2_input(const char *s, size_t len, int32 atttypmod);

/*
 * varchar2_input -- common guts of varchar2in and varchar2recv
 *
 * s is the input text of length len (may not be null-terminated)
 * atttypmod is the typmod value to apply
 *
 * If the input string is too long, raise an error
 *
 * Uses the C string to text conversion function, which is only appropriate
 * if VarChar and text are equivalent types.
 */

static VarChar *
varchar2_input(const char *s, size_t len, int32 atttypmod)
{
	VarChar		*result;		/* input data */
	size_t		maxlen;

	maxlen = atttypmod - VARHDRSZ;

	if (atttypmod >= (int32) VARHDRSZ && len > maxlen)
	{
		/* Verify that extra characters are spaces, and clip them off */
		size_t		mbmaxlen = pg_mbcharcliplen(s, len, maxlen);

		if (mbmaxlen < len)
			ereport(ERROR,
					(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
					 errmsg("value too long for type varchar2(%d)",
					 (int) maxlen)));

		len = mbmaxlen;
	}

	result = (VarChar *) cstring_to_text_with_len(s, len);
	return  result;
}

/*
 * Converts a C string to VARCHAR2 internal representation.  atttypmod
 * is the declared length of the type plus VARHDRSZ.
 */
Datum
varchar2in(PG_FUNCTION_ARGS)
{
	char	*s = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid		typelem = PG_GETARG_OID(1);
#endif
	int32	atttypmod = PG_GETARG_INT32(2);
	VarChar	*result;

	result = varchar2_input(s, strlen(s), atttypmod);
	PG_RETURN_VARCHAR_P(result);
}


/*
 * converts a VARCHAR2 value to a C string.
 *
 * Uses the text to C string conversion function, which is only appropriate
 * if VarChar and text are equivalent types.
 */
Datum
varchar2out(PG_FUNCTION_ARGS)
{
	Datum   txt = PG_GETARG_DATUM(0);

	PG_RETURN_CSTRING(TextDatumGetCString(txt));
}

/*
 * converts external binary format to varchar
 */
Datum
varchar2recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);	/* typmod of the receiving column */
	VarChar		*result;
	char		*str;							/* received data */
	int			nbytes;							/* length in bytes of recived data */

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);
	result = varchar2_input(str, nbytes, atttypmod);
	pfree(str);
	PG_RETURN_VARCHAR_P(result);
}


/*
 * varchar2send -- convert varchar2 to binary value
 */
Datum
varchar2send(PG_FUNCTION_ARGS)
{
	/* Exactly the same as textsend, so share code */
	return textsend(fcinfo);
}

/*
 * varchar2_transform()
 * Flatten calls to varchar's length coercion function that set the new maximum
 * length >= the previous maximum length.  We can ignore the isExplicit
 * argument, since that only affects truncation cases.
 *
 * just use varchar_transform()
 */

/*
 * Converts a VARCHAR2 type to the specified size.
 *
 * maxlen is the typmod, ie, declared length plus VARHDRSZ bytes.
 * isExplicit is true if this is for an explicit cast to varchar2(N).
 *
 * Truncation rules: for an explicit cast, silently truncate to the given
 * length; for an implicit cast, raise error if length limit is exceeded
 */
Datum
varchar2(PG_FUNCTION_ARGS)
{
	VarChar		*source = PG_GETARG_VARCHAR_PP(0);
	int32		typmod = PG_GETARG_INT32(1);
	bool		isExplicit = PG_GETARG_BOOL(2);
	int32		len,
				maxlen;
	size_t		maxmblen;
	char	   *s_data;

	len = VARSIZE_ANY_EXHDR(source);
	s_data = VARDATA_ANY(source);
	maxlen = typmod - VARHDRSZ;

	/* No work if typmod is invalid or supplied data fits it already */
	if (maxlen < 0 || len <= maxlen)
		PG_RETURN_VARCHAR_P(source);

	/* only reach here if string is too long... */

	/* truncate multibyte string preserving multibyte boundary */
	maxmblen = pg_mbcharcliplen(s_data, len, maxlen);

	/* error out if value too long unless it's an explicit cast */
	if (!isExplicit)
	{
		if (maxmblen < len)
			ereport(ERROR,
					(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
					 errmsg("value too long for type varchar2(%d)",
					 maxlen)));
	}

	PG_RETURN_VARCHAR_P((VarChar *) cstring_to_text_with_len(s_data,
															 maxmblen));
}


/*
 * varchar2typmodin -- type modifier input function
 *
 * just use varchartypmodin()
 */

/*
 * varchar2typmodout -- type modifier output function
 *
 * just use varchartypmodout()
 */
