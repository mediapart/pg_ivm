/*-------------------------------------------------------------------------
 *
 * pg_ivm.c
 *	  incremental view maintenance extension
 *    Routines for user interfaces and callback functions
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 2022, IVM Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_trigger_d.h"
#include "commands/trigger.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "parser/scansup.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/varlena.h"

#include "pg_ivm.h"
#include "nodes/print.h"

PG_MODULE_MAGIC;

static Oid pg_ivm_immv_id = InvalidOid;
static Oid pg_ivm_immv_pkey_id = InvalidOid;

void		_PG_init(void);

static void IvmXactCallback(XactEvent event, void *arg);
static void IvmSubXactCallback(SubXactEvent event, SubTransactionId mySubid,
							   SubTransactionId parentSubid, void *arg);
static void parseNameAndColumns(const char *string, List **names, List **colNames);

/* SQL callable functions */
PG_FUNCTION_INFO_V1(create_immv);
PG_FUNCTION_INFO_V1(refresh_immv);
PG_FUNCTION_INFO_V1(IVM_prevent_immv_change);

/*
 * Call back functions for cleaning up
 */
static void
IvmXactCallback(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_ABORT)
		AtAbort_IVM();
}

static void
IvmSubXactCallback(SubXactEvent event, SubTransactionId mySubid,
				   SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_ABORT_SUB)
		AtAbort_IVM();
}


/*
 * Module load callback
 */
void
_PG_init(void)
{
	RegisterXactCallback(IvmXactCallback, NULL);
	RegisterSubXactCallback(IvmSubXactCallback, NULL);
}

/*
 * Given a C string, parse it into a qualified relation name
 * followed by a optional parenthesized list of column names.
 */
static void
parseNameAndColumns(const char *string, List **names, List **colNames)
{
	char	   *rawname;
	char	   *ptr;
	char	   *ptr2;
	bool		in_quote;
	bool		has_colnames = false;
	List	   *cols;
	ListCell   *lc;

	/* We need a modifiable copy of the input string. */
	rawname = pstrdup(string);

	/* Scan to find the expected left paren; mustn't be quoted */
	in_quote = false;
	for (ptr = rawname; *ptr; ptr++)
	{
		if (*ptr == '"')
			in_quote = !in_quote;
		else if (*ptr == '(' && !in_quote)
		{
			has_colnames = true;
			break;
		}
	}

	/* Separate the name and parse it into a list */
	*ptr++ = '\0';
	*names = stringToQualifiedNameList(rawname);

	if (!has_colnames)
		goto end;

	/* Check for the trailing right parenthesis and remove it */
	ptr2 = ptr + strlen(ptr);
	while (--ptr2 > ptr)
	{
		if (!scanner_isspace(*ptr2))
			break;
	}
	if (*ptr2 != ')')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("expected a right parenthesis")));

	*ptr2 = '\0';

	if (!SplitIdentifierString(ptr, ',', &cols))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid name syntax")));

	if (list_length(cols) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("must specify at least one column name")));

	foreach(lc, cols)
	{
        char       *colname = lfirst(lc);
		*colNames = lappend(*colNames, makeString(pstrdup(colname)));
	}

end:
	pfree(rawname);
}

/*
 * User inerface for creating an IMMV
 */
Datum
create_immv(PG_FUNCTION_ARGS)
{
	text	*t_relname = PG_GETARG_TEXT_PP(0);
	text	*t_sql = PG_GETARG_TEXT_PP(1);
	char	*relname = text_to_cstring(t_relname);
	char	*sql = text_to_cstring(t_sql);
	List	*parsetree_list;
	RawStmt	*parsetree;
	Query	*query;
	QueryCompletion qc;
	List	*names = NIL;
	List	*colNames = NIL;

	ParseState *pstate = make_parsestate(NULL);
	CreateTableAsStmt *ctas;
	StringInfoData command_buf;

	parseNameAndColumns(relname, &names, &colNames);

	initStringInfo(&command_buf);
	appendStringInfo(&command_buf, "SELECT create_immv('%s' AS '%s');", relname, sql);
	appendStringInfo(&command_buf, "%s;", sql);
	pstate->p_sourcetext = command_buf.data;

	parsetree_list = pg_parse_query(sql);

	/* XXX: should we check t_sql before command_buf? */
	if (list_length(parsetree_list) != 1)
		elog(ERROR, "invalid view definition");

	parsetree = linitial_node(RawStmt, parsetree_list);

	ctas = makeNode(CreateTableAsStmt);
	ctas->query = parsetree->stmt;
#if defined(PG_VERSION_NUM) && (PG_VERSION_NUM >= 140000)
	ctas->objtype = OBJECT_MATVIEW;
#else
	ctas->relkind = OBJECT_MATVIEW;
#endif
	ctas->is_select_into = false;
	ctas->into = makeNode(IntoClause);
	ctas->into->rel = makeRangeVarFromNameList(names);
	ctas->into->colNames = colNames;
	ctas->into->accessMethod = NULL;
	ctas->into->options = NIL;
	ctas->into->onCommit = ONCOMMIT_NOOP;
	ctas->into->tableSpaceName = NULL;
	ctas->into->viewQuery = parsetree->stmt;
	ctas->into->skipData = false;

	query = transformStmt(pstate, (Node *)ctas);
	Assert(query->commandType == CMD_UTILITY && IsA(query->utilityStmt, CreateTableAsStmt));

	ExecCreateImmv(pstate, (CreateTableAsStmt *)query->utilityStmt, NULL, NULL, &qc);

	PG_RETURN_INT64(qc.nprocessed);
}

/*
 * Trigger function to prevent IMMV from being changed
 */
Datum
IVM_prevent_immv_change(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	Relation	rel = trigdata->tg_relation;

	if (!ImmvIncrementalMaintenanceIsEnabled())
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot change materialized view \"%s\"",
						RelationGetRelationName(rel))));

	return PointerGetDatum(NULL);
}

/*
 * User inerface for refreshing an IMMV
 */
Datum
refresh_immv(PG_FUNCTION_ARGS)
{
	text	*t_relname = PG_GETARG_TEXT_PP(0);
	bool	ispopulated = PG_GETARG_BOOL(1);
	char	*relname = text_to_cstring(t_relname);
	QueryCompletion qc;

	ExecRefreshImmv( relname, !(ispopulated), &qc);

	PG_RETURN_INT64(qc.nprocessed);
}

/*
 * Create triggers to prevent IMMV from being changed
 */
void
CreateChangePreventTrigger(Oid matviewOid)
{
	ObjectAddress	refaddr;
	ObjectAddress	address;
	CreateTrigStmt *ivm_trigger;

	int16 types[4] = {TRIGGER_TYPE_INSERT, TRIGGER_TYPE_DELETE,
					  TRIGGER_TYPE_UPDATE, TRIGGER_TYPE_TRUNCATE};
	int i;

	refaddr.classId = RelationRelationId;
	refaddr.objectId = matviewOid;
	refaddr.objectSubId = 0;

	ivm_trigger = makeNode(CreateTrigStmt);
	ivm_trigger->relation = NULL;
	ivm_trigger->row = false;

	ivm_trigger->timing = TRIGGER_TYPE_BEFORE;
	ivm_trigger->trigname = "IVM_prevent_immv_change";
	ivm_trigger->funcname = SystemFuncName("IVM_prevent_immv_change");
	ivm_trigger->columns = NIL;
	ivm_trigger->transitionRels = NIL;
	ivm_trigger->whenClause = NULL;
	ivm_trigger->isconstraint = false;
	ivm_trigger->deferrable = false;
	ivm_trigger->initdeferred = false;
	ivm_trigger->constrrel = NULL;
	ivm_trigger->args = NIL;

	for (i = 0; i < 4; i++)
	{
		ivm_trigger->events = types[i];
		address = CreateTrigger(ivm_trigger, NULL, matviewOid, InvalidOid, InvalidOid,
							 InvalidOid, InvalidOid, InvalidOid, NULL, true, false);

		recordDependencyOn(&address, &refaddr, DEPENDENCY_AUTO);
	}

	/* Make changes-so-far visible */
	CommandCounterIncrement();
}

/*
 * Get relid of pg_ivm_immv
 */
Oid
PgIvmImmvRelationId(void)
{
	if (!OidIsValid(pg_ivm_immv_id))
		pg_ivm_immv_id = get_relname_relid("pg_ivm_immv", PG_CATALOG_NAMESPACE);

	return pg_ivm_immv_id;
}

/*
 * Get relid of pg_ivm_immv's primary key
 */
Oid
PgIvmImmvPrimaryKeyIndexId(void)
{
	if (!OidIsValid(pg_ivm_immv_pkey_id))
		pg_ivm_immv_pkey_id = get_relname_relid("pg_ivm_immv_pkey", PG_CATALOG_NAMESPACE);

	return pg_ivm_immv_pkey_id;
}

