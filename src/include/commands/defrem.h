/*-------------------------------------------------------------------------
 *
 * defrem.h
 *	  POSTGRES define and remove utility definitions.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/defrem.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DEFREM_H
#define DEFREM_H

#include "catalog/objectaddress.h"
#include "nodes/parsenodes.h"
#include "utils/array.h"
#ifdef ADB
#include "tcop/utility.h"
#endif

/* commands/dropcmds.c */
extern void RemoveObjects(DropStmt *stmt);

/* commands/indexcmds.c */
extern ObjectAddress DefineIndex(Oid relationId,
			IndexStmt *stmt,
			Oid indexRelationId,
			bool is_alter_table,
			bool check_rights,
			bool skip_build,
			bool quiet);
extern Oid	ReindexIndex(RangeVar *indexRelation, int options);
extern Oid	ReindexTable(RangeVar *relation, int options);
extern void ReindexMultipleTables(const char *objectName, ReindexObjectType objectKind,
					  int options);
extern char *makeObjectName(const char *name1, const char *name2,
			   const char *label);
extern char *ChooseRelationName(const char *name1, const char *name2,
				   const char *label, Oid namespaceid);
extern bool CheckIndexCompatible(Oid oldId,
					 char *accessMethodName,
					 List *attributeList,
					 List *exclusionOpNames);
extern Oid	GetDefaultOpClass(Oid type_id, Oid am_id);

/* commands/functioncmds.c */
extern ObjectAddress CreateFunction(CreateFunctionStmt *stmt, const char *queryString);
extern void RemoveFunctionById(Oid funcOid);
extern void SetFunctionReturnType(Oid funcOid, Oid newRetType);
extern void SetFunctionArgType(Oid funcOid, int argIndex, Oid newArgType);
extern ObjectAddress AlterFunction(AlterFunctionStmt *stmt);
extern ObjectAddress CreateCast(CreateCastStmt *stmt);
extern void DropCastById(Oid castOid);
extern ObjectAddress CreateTransform(CreateTransformStmt *stmt);
extern void DropTransformById(Oid transformOid);
extern void IsThereFunctionInNamespace(const char *proname, int pronargs,
						   oidvector *proargtypes, Oid nspOid);
extern void ExecuteDoStmt(DoStmt *stmt);
extern Oid	get_cast_oid(Oid sourcetypeid, Oid targettypeid, bool missing_ok);
extern Oid	get_transform_oid(Oid type_id, Oid lang_id, bool missing_ok);
extern void interpret_function_parameter_list(List *parameters,
								  Oid languageOid,
								  bool is_aggregate,
								  const char *queryString,
								  oidvector **parameterTypes,
								  ArrayType **allParameterTypes,
								  ArrayType **parameterModes,
								  ArrayType **parameterNames,
								  List **parameterDefaults,
								  Oid *variadicArgType,
								  Oid *requiredResultType);

/* commands/operatorcmds.c */
extern ObjectAddress DefineOperator(List *names, List *parameters);
extern void RemoveOperatorById(Oid operOid);
extern ObjectAddress AlterOperator(AlterOperatorStmt *stmt);

/* commands/aggregatecmds.c */
extern ObjectAddress DefineAggregate(List *name, List *args, bool oldstyle,
				List *parameters, const char *queryString);

/* commands/opclasscmds.c */
extern ObjectAddress DefineOpClass(CreateOpClassStmt *stmt);
extern ObjectAddress DefineOpFamily(CreateOpFamilyStmt *stmt);
extern Oid	AlterOpFamily(AlterOpFamilyStmt *stmt);
extern void RemoveOpClassById(Oid opclassOid);
extern void RemoveOpFamilyById(Oid opfamilyOid);
extern void RemoveAmOpEntryById(Oid entryOid);
extern void RemoveAmProcEntryById(Oid entryOid);
extern void IsThereOpClassInNamespace(const char *opcname, Oid opcmethod,
						  Oid opcnamespace);
extern void IsThereOpFamilyInNamespace(const char *opfname, Oid opfmethod,
						   Oid opfnamespace);
extern Oid	get_opclass_oid(Oid amID, List *opclassname, bool missing_ok);
extern Oid	get_opfamily_oid(Oid amID, List *opfamilyname, bool missing_ok);

/* commands/tsearchcmds.c */
extern ObjectAddress DefineTSParser(List *names, List *parameters);
extern void RemoveTSParserById(Oid prsId);

extern ObjectAddress DefineTSDictionary(List *names, List *parameters);
extern void RemoveTSDictionaryById(Oid dictId);
extern ObjectAddress AlterTSDictionary(AlterTSDictionaryStmt *stmt);

extern ObjectAddress DefineTSTemplate(List *names, List *parameters);
extern void RemoveTSTemplateById(Oid tmplId);

extern ObjectAddress DefineTSConfiguration(List *names, List *parameters,
					  ObjectAddress *copied);
extern void RemoveTSConfigurationById(Oid cfgId);
extern ObjectAddress AlterTSConfiguration(AlterTSConfigurationStmt *stmt);

extern text *serialize_deflist(List *deflist);
extern List *deserialize_deflist(Datum txt);

/* commands/foreigncmds.c */
extern ObjectAddress AlterForeignServerOwner(const char *name, Oid newOwnerId);
extern void AlterForeignServerOwner_oid(Oid, Oid newOwnerId);
extern ObjectAddress AlterForeignDataWrapperOwner(const char *name, Oid newOwnerId);
extern void AlterForeignDataWrapperOwner_oid(Oid fwdId, Oid newOwnerId);
extern ObjectAddress CreateForeignDataWrapper(CreateFdwStmt *stmt);
extern ObjectAddress AlterForeignDataWrapper(AlterFdwStmt *stmt);
extern void RemoveForeignDataWrapperById(Oid fdwId);
extern ObjectAddress CreateForeignServer(CreateForeignServerStmt *stmt);
extern ObjectAddress AlterForeignServer(AlterForeignServerStmt *stmt);
extern void RemoveForeignServerById(Oid srvId);
extern ObjectAddress CreateUserMapping(CreateUserMappingStmt *stmt);
extern ObjectAddress AlterUserMapping(AlterUserMappingStmt *stmt);
extern Oid	RemoveUserMapping(DropUserMappingStmt *stmt);
extern void RemoveUserMappingById(Oid umId);
extern void CreateForeignTable(CreateForeignTableStmt *stmt, Oid relid);
extern void ImportForeignSchema(ImportForeignSchemaStmt *stmt);
extern Datum transformGenericOptions(Oid catalogId,
						Datum oldOptions,
						List *options,
						Oid fdwvalidator);

/* commands/amcmds.c */
extern ObjectAddress CreateAccessMethod(CreateAmStmt *stmt);
extern void RemoveAccessMethodById(Oid amOid);
extern Oid	get_index_am_oid(const char *amname, bool missing_ok);
extern Oid	get_am_oid(const char *amname, bool missing_ok);
extern char *get_am_name(Oid amOid);

/* support routines in commands/define.c */

extern char *defGetString(DefElem *def);
extern double defGetNumeric(DefElem *def);
extern bool defGetBoolean(DefElem *def);
extern int32 defGetInt32(DefElem *def);
extern int64 defGetInt64(DefElem *def);
extern List *defGetQualifiedName(DefElem *def);
extern TypeName *defGetTypeName(DefElem *def);
extern int	defGetTypeLength(DefElem *def);
extern DefElem *defWithOids(bool value);

#ifdef ADB
/* commands/auxiliarytablecmds.c */
extern void InsertAuxClassTuple(Oid auxrelid, Oid relid, AttrNumber attnum);
extern void RemoveAuxClassTuple(Oid auxrelid, Oid relid, AttrNumber attnum);
extern Oid LookupAuxRelation(Oid relid, AttrNumber attnum);
extern Oid LookupAuxMasterRel(Oid auxrelid, AttrNumber *attnum);
extern bool RelationIdGetAuxAttnum(Oid auxrelid, AttrNumber *attnum);
extern bool HasAuxRelation(Oid relid);
extern void ExecPaddingAuxDataStmt(PaddingAuxDataStmt *stmt, StringInfo msg);
extern void ExecCreateAuxStmt(CreateAuxStmt *auxstmt,
							  const char *queryString,
							  ProcessUtilityContext context,
							  DestReceiver *dest,
							  bool sentToRemote,
							  char *completionTag);
extern void PaddingAuxDataOfMaster(Relation master);
#if 0
extern List *QueryRewriteAuxStmt(Query *auxquery);
#endif
extern void RelationBuildAuxiliary(Relation rel);
extern Bitmapset *MakeAuxMainRelResultAttnos(Relation rel);
extern List *MakeMainRelTargetForAux(Relation main_rel, Relation aux_rel, Index relid, bool target_entry);

#define RelationIdHasAuxRelation(relid) \
	HasAuxRelation(relid)

#define RelationHasAuxRelation(relation) \
	HasAuxRelation(RelationGetRelid(relation))

#define RelationIdIsAuxiliary(relid) \
	RelationIdGetAuxAttnum(relid, NULL)

#define RelationIsAuxiliary(relation) \
	RelationIdGetAuxAttnum(RelationGetRelid(relation), NULL)

#define RelationGetAuxAttnum(relation, auxattnum) \
	RelationIdGetAuxAttnum(RelationGetRelid(relation), (auxattnum))

#endif

#endif   /* DEFREM_H */
