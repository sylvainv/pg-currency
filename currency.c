/*
 * PostgreSQL type definitions for currency type
 * Written by Sam Vilain
 * sam.vilain@adioso.com
 *
 * contrib/currency/currency.c
 */

#include "postgres.h"

#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>

#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "executor/spi.h"
#include "access/xact.h"

#include "fmgr.h"

/*
 * in principle, we could select oids from the various catalog tables
 * and use the FCI to call the appropriate functions by Oid, but this
 * saves that SPI call and other questions about invalidating/caching
 * that retrieved Oid.
 */
#include "tla.h"

/*
 * This type combines int8 fixed-point numbers with a currency code,
 * to provide automatically converted values via an auxilliary lookup
 * table.
 */

/* IO methods */
Datum		currency_in_cstring(PG_FUNCTION_ARGS);
Datum		currency_out_cstring(PG_FUNCTION_ARGS);
//Datum		currency_in_text(PG_FUNCTION_ARGS);
//Datum		currency_out_text(PG_FUNCTION_ARGS);

/* comparisons */
Datum		currency_eq(PG_FUNCTION_ARGS);
Datum		currency_ne(PG_FUNCTION_ARGS);
Datum		currency_lt(PG_FUNCTION_ARGS);
Datum		currency_le(PG_FUNCTION_ARGS);
Datum		currency_ge(PG_FUNCTION_ARGS);
Datum		currency_gt(PG_FUNCTION_ARGS);

/* necessary typecasting methods */
//Datum		currency_in_text(PG_FUNCTION_ARGS);
//Datum		currency_out_text(PG_FUNCTION_ARGS);
//Datum		currency_in_money(PG_FUNCTION_ARGS);
//Datum		currency_out_money(PG_FUNCTION_ARGS);

static MemoryContext CurrencyCacheContext = NULL;

static void *cc_palloc(size_t size);
static char *cc_pstrdup(const char *string);

#define numeric_oid 1700
#define numeric_in 1701
#define numeric_out 1702
#define numeric_round_scale 1707
#define numeric_eq 1718
#define numeric_ne 1719
#define numeric_gt 1720
#define numeric_ge 1721
#define numeric_lt 1722
#define numeric_le 1723
#define numeric_add 1724
#define numeric_sub 1725
#define numeric_mul 1726
#define numeric_div 1727
#define numeric_cmp 1769
#define numeric_uplus 1915
#define numeric_uminus 1771
#define hash_numeric 432
#define hashint2 449

/* memory/heap structure (not for binary marshalling) */
typedef struct currency
{
	int32 vl_len_;	/* varlena header */
        int16 currency_code;
	char numeric[]; /* numeric data, EXCLUDING the varlena header */
} currency;

#define alloc_varlena( var, size ) \
	var = palloc( size ); \
	SET_VARSIZE( var, size );

static void *
cc_palloc(size_t size)
{
	return MemoryContextAlloc(CurrencyCacheContext, size);
}


static char *
cc_pstrdup(const char *string)
{
	return MemoryContextStrdup(CurrencyCacheContext, string);
}

currency* make_currency(struct tv* numeric, int16 currency_code) {
	currency* newval;

	alloc_varlena(
		newval,
		VARSIZE( numeric ) + offsetof(currency, numeric) - VARHDRSZ
		);
	memcpy( &newval->numeric,
		DatumGetPointer( numeric ) + VARHDRSZ,
		VARSIZE( numeric ) - VARHDRSZ );
	newval->currency_code = currency_code;

	return newval;
}

/* handy little function for dumping chunks of memory as hex :) */
char* dump_hex(void* ptr, int len)
{
	char* hexa;
	int i;
	char* x;
	unsigned char* nx;

	hexa = palloc( len * 2 + 1 );
	x = hexa;
	nx = ptr;
	for ( i = 0; i < len; i++ ) {
		sprintf(x, "%.2x", *nx);
		x += 2;
		nx++;
	}
	return hexa;
}

/* creating a currency from a string */
currency* parse_currency(char* str)
{
	char* number = palloc(strlen(str)+1);
	Datum numeric_val;
	char code[4];
	char fail[2] = "";
	currency *newval;
	int16 currency_code;

	// use sscanf, so that whitespace between number and currency
	// code can be optional.
	if (sscanf(str, " %[0-9.,-] %3c %1c", number, (char*)&code, (char*)&fail) != 2) {
		elog(ERROR, "bad currency value '%s'", str);
	}
	code[3] = '\0';

	// use numeric_in to parse the numeric
	numeric_val = OidFunctionCall3(
		numeric_in,
		CStringGetDatum( number ),
		numeric,
		-1  /* typmod */
		);
	if (!(currency_code = parse_tla(&code))) {
		elog(ERROR, "bad currency code '%s'", &code);
	}

	newval = make_currency(numeric_val, currency_code);

	pfree(numeric_val);

	return newval;
}

struct varlena* _currency_numeric(currency* amount) {
	struct varlena* tv;

	alloc_varlena(
		tv,
		VARSIZE( amount ) - offsetof(currency, numeric) + VARHDRSZ
		);
	memcpy( (char*)tv + VARHDRSZ,
		&amount->numeric,
		VARSIZE( amount ) - offsetof(currency, numeric)
		);

	return tv;
}

char* emit_currency(currency* amount) {
	char* res;
	char* outstr;
	char* c;

	/* construct a varlena structure so we can call the
	 * numeric_out function and have a happy life */
	struct varlena* tv = _currency_numeric(amount);

	outstr = OidFunctionCall1( numeric_out, PointerGetDatum( tv ) );

	pfree(tv);

	res = palloc( strlen(outstr) + 5 );
	memcpy( res, outstr, strlen(outstr)+1 );
	pfree(outstr);
	c = res + strlen(res);
	*c++ = ' ';
	emit_tla_buf( amount->currency_code, c );
	c += 3;
	c = '\0';

	return res;
}


/*
 * Pg bindings
 */

/* input function: C string */
PG_FUNCTION_INFO_V1(currency_in_cstring);
Datum
currency_in_cstring(PG_FUNCTION_ARGS)
{
	char *str = PG_GETARG_CSTRING(0);
	currency *result = parse_currency(str);
	if (!result)
		PG_RETURN_NULL();

	PG_RETURN_POINTER(result);
}/* output function: C string */

PG_FUNCTION_INFO_V1(currency_out_cstring);
Datum
currency_out_cstring(PG_FUNCTION_ARGS)
{
	currency* amount = (void*)PG_GETARG_POINTER(0);
	char *result;
	result = emit_currency(amount);

	PG_RETURN_CSTRING(result);
}

static TransactionId ccc_txid = -1;
static CommandId ccc_cmdid = -1;

typedef struct ccc_ent
{
	int16 currency_code;
	int16 currency_minor;
	struct varlena* currency_rate;
	char* currency_symbol;
} ccc_ent;

static ccc_ent* currency_code_cache = 0;
static int ccc_size;

inline void update_currency_code_cache()
{
	if (ccc_cmdid != GetCurrentCommandId(false) ||
	    ccc_txid != GetCurrentTransactionId()
		) {
		if (!_update_cc_cache()) {
			elog(ERROR, "failed to update currency code cache");
		}
	}
}

int _update_cc_cache() {
	int res, i;
	HeapTuple tuple;
	TupleDesc tupdesc;
	Datum attr;
	Oid typoutput_sym;
	bool junk, isnull;
	char* outputstr;
	struct tv *numeric;

	if (SPI_connect() == SPI_ERROR_CONNECT) {
		elog(ERROR, "failed to connect to SPI");
		return 0;
	}
	res = SPI_execute(
		"select"
		" code, minor, rate, symbol, is_exchange"
		" from currency_rate"
		" order by is_exchange desc, code"
		, true
		, 1000
		);
	if (res != SPI_OK_SELECT) {
		elog(ERROR, "failed to fetch currency codes");
		return 0;
	}

	if (CurrencyCacheContext == NULL) {
		CurrencyCacheContext = AllocSetContextCreate(
			TopMemoryContext,
			"CurrencyCacheContext",
			ALLOCSET_DEFAULT_MINSIZE,
			ALLOCSET_DEFAULT_INITSIZE,
			ALLOCSET_DEFAULT_MAXSIZE
			);
	}
	else {
		MemoryContextReset(CurrencyCacheContext);
	}

	currency_code_cache = cc_palloc(sizeof(ccc_ent) * SPI_processed);
	ccc_size = SPI_processed;

	/* results are in SPI_tuptable */
	tupdesc = SPI_tuptable->tupdesc;
	getTypeOutputInfo(
		tupdesc->attrs[3]->atttypid,
		&typoutput_sym, &junk
		);

	for (i = 0; i < SPI_processed; i++) {
		/* currency_code */
		tuple = SPI_tuptable->vals[i];
		attr = heap_getattr(tuple, 1, tupdesc, &isnull);
		currency_code_cache[i].currency_code = attr;

		/* minor */
		attr = heap_getattr(tuple, 2, tupdesc, &isnull);
		currency_code_cache[i].currency_minor = attr;

		/* symbol */
		attr = heap_getattr(tuple, 4, tupdesc, &isnull);
		if (isnull) {
			currency_code_cache[i].currency_symbol = 0;
		}
		else {
			outputstr = OidOutputFunctionCall(typoutput_sym, attr);
			currency_code_cache[i].currency_symbol = cc_pstrdup(outputstr);
		}
		
		/* rate */
		attr = heap_getattr(tuple, 3, tupdesc, &isnull);
		/* this seems to help */
		numeric = OidFunctionCall1( numeric_uplus, attr );
		currency_code_cache[i].currency_rate = cc_palloc( VARSIZE(numeric) );
		memcpy(currency_code_cache[i].currency_rate,
		       numeric,
		       VARSIZE(numeric));

		/* is_exchange */
		attr = heap_getattr(tuple, 5, tupdesc, &isnull);
		if (attr && i != 0) {
			elog(ERROR, "multiple currencies marked as exchange currency");
		}
		else if (!attr && i == 0) {
			elog(ERROR, "you must mark one currency as the exchange currency");
		}

	}

	SPI_finish();

	ccc_cmdid = GetCurrentCommandId(false);
	ccc_txid = GetCurrentTransactionId();
	return ccc_size;
}

ccc_ent* lookup_currency_code(int16 currency_code) {
	int max, min, i, f;

	if (currency_code_cache[0].currency_code == currency_code) {
		return currency_code_cache;
	}

	min = 1;
	max = ccc_size - 1;

	while (min <= max) {
		i = (min + max) >> 1;
		f = currency_code_cache[i].currency_code;
		if ( f == currency_code ) {
			return &currency_code_cache[i];
		}
		else if ( f < currency_code ) {
			min = i + 1;
		}
		else {
			max = i - 1;
		}
	}
	return 0;
}

PG_FUNCTION_INFO_V1(currency_format);
Datum
currency_format(PG_FUNCTION_ARGS)
{
	currency* amount = (void*)PG_GETARG_POINTER(0);
	char *result;
	char *number;
	char *x;
	ccc_ent *info;
	struct varlena* numeric;
	struct varlena* rounded;

	update_currency_code_cache();
	info = lookup_currency_code( amount->currency_code );
	if ( !info )
		elog(ERROR, "currency code '%s' not in currency_rate table",
		     emit_tla( amount->currency_code ));

	numeric = _currency_numeric(amount);
	rounded = OidFunctionCall2(
		numeric_round_scale, PointerGetDatum( numeric ),
		info->currency_minor
		);
	number = OidFunctionCall1( numeric_out, PointerGetDatum( rounded ) );
	pfree(numeric);
	pfree(rounded);

	if (info->currency_symbol) {
		result = palloc( strlen(info->currency_symbol) + strlen(number) + 2 );
		strcpy( result, info->currency_symbol );
		x = result + strlen(result);
	}
	else {
		result = palloc( strlen(number) + 5 );
		emit_tla_buf( info->currency_code, result );
		x = result + 3;
	}
	*x++ = ' ';
	strcpy(x, number);
	pfree(number);

	PG_RETURN_CSTRING(result);
}

/* convert a currency to a neutral NUMERIC value */
struct varlena* currency_neutral(currency* amount) {
	struct varlena* amount_num = _currency_numeric(amount);
	struct varlena* neutral;
	ccc_ent* cc_info;

	cc_info = lookup_currency_code(amount->currency_code);
	if (!cc_info)
		elog(ERROR, "currency code '%s' not in currency_rate table",
		     emit_tla( amount->currency_code ));

	if (cc_info == currency_code_cache) {
		return amount_num;
	}
	else {
		neutral = OidFunctionCall2(
			numeric_mul,
			_currency_numeric(amount),
			PointerGetDatum(cc_info->currency_rate)
			);
		pfree(amount_num);

		return neutral;
	}
}

PG_FUNCTION_INFO_V1(currency_convert);
Datum
currency_convert(PG_FUNCTION_ARGS)
{
	currency* amount = (void*)PG_GETARG_POINTER(0);
	int16 target_code = PG_GETARG_DATUM(1);

	struct varlena* neutral;
	struct varlena* target;
	ccc_ent *cc_to;
	currency* newval;

	update_currency_code_cache();
	neutral = currency_neutral(amount);

	cc_to = lookup_currency_code(target_code);
	if (!cc_to)
		elog(ERROR, "currency code '%s' not in currency_rate table",
		     emit_tla( target_code ));

	if (cc_to == currency_code_cache) {
		target = neutral;
	}
	else {
		target = OidFunctionCall2(
			numeric_div,
			PointerGetDatum(neutral),
			PointerGetDatum(cc_to->currency_rate)
			);
		pfree(neutral);
	}

	newval = make_currency(target, target_code);

	pfree(target);
	return newval;

}

PG_FUNCTION_INFO_V1(currency_code);
Datum
currency_code(PG_FUNCTION_ARGS)
{
	currency* amount = (void*)PG_GETARG_POINTER(0);
	int16 currency_code = amount->currency_code;

	PG_RETURN_DATUM(currency_code);
}

PG_FUNCTION_INFO_V1(currency_value);
Datum
currency_value(PG_FUNCTION_ARGS)
{
	currency* amount = (void*)PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(_currency_numeric(amount));
}

PG_FUNCTION_INFO_V1(currency_compose);
Datum
currency_compose(PG_FUNCTION_ARGS)
{
	struct tv* number = PG_GETARG_POINTER(0);
	int16 currency_code = PG_GETARG_DATUM(1);
	currency* newval;

	PG_RETURN_POINTER( make_currency( number, currency_code ));
}

#if PG_VERSION_NUM >= 90100
#define numeric_cash 3824
#else
#define cash_in 886
#endif

PG_FUNCTION_INFO_V1(currency_money);
Datum
currency_money(PG_FUNCTION_ARGS)
{
	currency* amount = (void*)PG_GETARG_POINTER(0);
	int16 target_code;
	struct tv* neutral;
	ccc_ent* cc_from;
#ifndef numeric_cash
	char* outstr;
#endif

	update_currency_code_cache();

	neutral = currency_neutral(amount);

#ifdef numeric_cash
	PG_RETURN_POINTER( OidFunctionCall1( numeric_cash, neutral ) );
#else
	// else go through a C string, of course :)
	outstr = OidFunctionCall1( numeric_out, PointerGetDatum( neutral ) );
	PG_RETURN_POINTER( OidFunctionCall1( cash_in, CStringGetDatum(outstr) ) );
#endif
}

PG_FUNCTION_INFO_V1(currency_numeric);
Datum
currency_numeric(PG_FUNCTION_ARGS)
{
	currency* amount = (void*)PG_GETARG_POINTER(0);
	int16 target_code;
	struct tv *neutral, *rounded;
	ccc_ent* neutral_info;

	update_currency_code_cache();
	neutral_info = currency_code_cache;

	neutral = currency_neutral(amount);
	rounded = OidFunctionCall2(
		numeric_round_scale, PointerGetDatum( neutral ),
		neutral_info->currency_minor
		);
	pfree(neutral);
	PG_FREE_IF_COPY(amount, 0);

	PG_RETURN_POINTER( rounded );
}

/* a <=>-style compare function; caller is expected to have called
 * update_currency_code_cache() */
int currency_cmp(currency* a, currency* b)
{
	int rv;
	struct tv *a_n, *b_n;
	if (a->currency_code == b->currency_code) {
		a_n = _currency_numeric(a);
		b_n = _currency_numeric(b);
	}
	else {
		a_n = currency_neutral(a);
		b_n = currency_neutral(b);
	}
	rv = OidFunctionCall2(
		numeric_cmp,
		PointerGetDatum( a_n ),
		PointerGetDatum( b_n )
		);
	pfree(a_n);
	pfree(b_n);
	return rv;
}

PG_FUNCTION_INFO_V1(currency_eq);
Datum
currency_eq(PG_FUNCTION_ARGS)
{
	currency* a = (void*)PG_GETARG_POINTER(0);
	currency* b = (void*)PG_GETARG_POINTER(1);
	update_currency_code_cache();
	int diff = currency_cmp(a, b);
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(diff == 0);
}

PG_FUNCTION_INFO_V1(currency_ne);
Datum
currency_ne(PG_FUNCTION_ARGS)
{
	currency* a = (void*)PG_GETARG_POINTER(0);
	currency* b = (void*)PG_GETARG_POINTER(1);
	update_currency_code_cache();
	int diff = currency_cmp(a, b);
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(diff != 0);
}

PG_FUNCTION_INFO_V1(currency_le);
Datum
currency_le(PG_FUNCTION_ARGS)
{
	currency* a = (void*)PG_GETARG_POINTER(0);
	currency* b = (void*)PG_GETARG_POINTER(1);
	update_currency_code_cache();
	int diff = currency_cmp(a, b);
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(diff <= 0);
}

PG_FUNCTION_INFO_V1(currency_lt);
Datum
currency_lt(PG_FUNCTION_ARGS)
{
	currency* a = (void*)PG_GETARG_POINTER(0);
	currency* b = (void*)PG_GETARG_POINTER(1);
	update_currency_code_cache();
	int diff = currency_cmp(a, b);
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(diff < 0);
}

PG_FUNCTION_INFO_V1(currency_ge);
Datum
currency_ge(PG_FUNCTION_ARGS)
{
	currency* a = (void*)PG_GETARG_POINTER(0);
	currency* b = (void*)PG_GETARG_POINTER(1);
	update_currency_code_cache();
	int diff = currency_cmp(a, b);
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(diff >= 0);
}

PG_FUNCTION_INFO_V1(currency_gt);
Datum
currency_gt(PG_FUNCTION_ARGS)
{
	currency* a = (void*)PG_GETARG_POINTER(0);
	currency* b = (void*)PG_GETARG_POINTER(1);
	update_currency_code_cache();
	int diff = currency_cmp(a, b);
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(diff > 0);
}

PG_FUNCTION_INFO_V1(currency_btcmp);
Datum
currency_btcmp(PG_FUNCTION_ARGS)
{
	currency* a = (void*)PG_GETARG_POINTER(0);
	currency* b = (void*)PG_GETARG_POINTER(1);
	update_currency_code_cache();
	int diff = currency_cmp(a, b);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_INT32(diff);
}

PG_FUNCTION_INFO_V1(currency_hash);
Datum
currency_hash(PG_FUNCTION_ARGS)
{
	currency* amount = (void*)PG_GETARG_POINTER(0);
	struct tv* numeric;
	int32 numeric_hash;
	update_currency_code_cache();
	numeric = currency_neutral(amount);
	numeric_hash = OidFunctionCall1(hash_numeric, numeric);
	pfree(numeric);
	PG_FREE_IF_COPY(amount, 0);

	PG_RETURN_INT32(numeric_hash);
}

currency* currency_math2(int operator, currency *arg1, currency* arg2)
{
	int16 currency_code;
	struct tv *arg1_num, *arg2_num;
	struct tv *result_num;
	currency *result;

	if (arg1->currency_code != arg2->currency_code) {
		currency_code = currency_code_cache[0].currency_code;
		arg1_num = currency_neutral(arg1);
		arg2_num = currency_neutral(arg2);
	}
	else {
		currency_code = arg1->currency_code;
		arg1_num = _currency_numeric(arg1);
		arg2_num = _currency_numeric(arg2);
	}

	result_num = OidFunctionCall2(
		operator,
		PointerGetDatum(arg1_num), PointerGetDatum(arg2_num)
		);

	pfree(arg1_num);
	pfree(arg2_num);

	result = make_currency(result_num, currency_code);
	pfree(result_num);
	return result;
}

PG_FUNCTION_INFO_V1(currency_add);
Datum
currency_add(PG_FUNCTION_ARGS)
{
	currency* augend = (void*)PG_GETARG_POINTER(0);
	currency* addend = (void*)PG_GETARG_POINTER(1);

	currency* sum;

	update_currency_code_cache();

	sum = currency_math2(numeric_add, augend, addend);

	PG_FREE_IF_COPY(augend, 0);
	PG_FREE_IF_COPY(addend, 1);

	PG_RETURN_POINTER(sum);
}

PG_FUNCTION_INFO_V1(currency_sub);
Datum
currency_sub(PG_FUNCTION_ARGS)
{
	currency* minuend = (void*)PG_GETARG_POINTER(0);
	currency* subtrahend = (void*)PG_GETARG_POINTER(1);

	currency* difference;

	update_currency_code_cache();

	difference = currency_math2(numeric_sub, minuend, subtrahend);

	PG_FREE_IF_COPY(minuend, 0);
	PG_FREE_IF_COPY(subtrahend, 1);

	PG_RETURN_POINTER(difference);
}

PG_FUNCTION_INFO_V1(currency_mul);
Datum
currency_mul(PG_FUNCTION_ARGS)
{
	bool num_first = get_fn_expr_argtype(fcinfo->flinfo, 0) == numeric_oid;

	currency* amount = (void*)PG_GETARG_POINTER( num_first ? 1 : 0 );
	struct tv* factor = (void*)PG_GETARG_POINTER( num_first ? 0 : 1 );
	struct tv *amount_num, *product_num;
	currency* product;

	amount_num = _currency_numeric(amount);
	product_num = OidFunctionCall2(
		numeric_mul,
		PointerGetDatum(amount_num),
		PointerGetDatum(factor)
		);

	product = make_currency( product_num, amount->currency_code );

	pfree(amount_num);
	pfree(product_num);

	PG_FREE_IF_COPY(amount, num_first ? 1 : 0);
	PG_FREE_IF_COPY(factor, num_first ? 0 : 1);

	PG_RETURN_POINTER(product);
}

PG_FUNCTION_INFO_V1(currency_div);
Datum
currency_div(PG_FUNCTION_ARGS)
{
	bool return_currency =
		get_fn_expr_argtype(fcinfo->flinfo, 1) == numeric_oid;

	currency* dividend = (void*)PG_GETARG_POINTER( 0 );
	currency *divisor, *quotient;
	struct tv *dividend_num, *divisor_num, *quotient_num;

	if (return_currency) {
		// dividing a currency by a numeric
		dividend_num = _currency_numeric(dividend);
		divisor_num = (void*)PG_GETARG_POINTER(1);
	}
	else {
		// dividing two currencies
		divisor = (void*)PG_GETARG_POINTER(1);
		if (dividend->currency_code != divisor->currency_code) {
			update_currency_code_cache();
			dividend_num = currency_neutral(dividend);
			divisor_num = currency_neutral(divisor);
		}
		else {
			dividend_num = _currency_numeric(dividend);
			divisor_num = _currency_numeric(divisor);
		}
	}

	quotient_num = OidFunctionCall2(
		numeric_div,
		PointerGetDatum(dividend_num),
		PointerGetDatum(divisor_num)
		);

	PG_FREE_IF_COPY(dividend, 0);
	pfree(dividend_num);

	if (return_currency) {
		quotient = make_currency(
			quotient_num,
			dividend->currency_code
			);
		PG_FREE_IF_COPY(divisor_num, 1);
		pfree(quotient_num);
		PG_RETURN_POINTER(quotient);
	}
	else {
		pfree(divisor_num);
		PG_FREE_IF_COPY(divisor, 1);
		PG_RETURN_POINTER(quotient_num);
	}
}

PG_FUNCTION_INFO_V1(currency_uplus);
Datum
currency_uplus(PG_FUNCTION_ARGS)
{
	currency* amount = (void*)PG_GETARG_POINTER(0);
	struct tv* num = _currency_numeric(amount);
	currency* copy = make_currency(num, amount->currency_code);

	PG_FREE_IF_COPY(amount, 0);
	pfree(num);

	PG_RETURN_POINTER(copy);
}

PG_FUNCTION_INFO_V1(currency_uminus);
Datum
currency_uminus(PG_FUNCTION_ARGS)
{
	currency* amount = (void*)PG_GETARG_POINTER(0);
	struct tv* num = _currency_numeric(amount);
	struct tv* negnum = OidFunctionCall1( numeric_uminus, num );
	currency* neg = make_currency(negnum, amount->currency_code);

	PG_FREE_IF_COPY(amount, 0);
	pfree(num);
	pfree(negnum);

	PG_RETURN_POINTER(neg);
}
