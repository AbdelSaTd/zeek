// See the file "COPYING" in the main distribution directory for copyright.

#include "UseDefs.h"
#include "Reduce.h"
#include "Expr.h"
#include "Stmt.h"
#include "Desc.h"
#include "Reporter.h"
#include "ScriptAnaly.h"


void UseDefSet::Dump() const
	{
	for ( const auto& u : IterateOver() )
		printf(" %s", u->Name());
	}

UseDefs::UseDefs(Stmt* _body, Reducer* _rc)
	{
	body = _body;
	rc = _rc;
	}

UseDefs::~UseDefs()
	{
	}

void UseDefs::Analyze()
	{
	// Start afresh.
	use_defs_map.clear();
	UDs_are_copies.clear();
	stmts.clear();
	successor.clear();
	successor2.clear();

	(void) PropagateUDs(body, nullptr, nullptr, false);
	}


void UseDefs::RemoveUnused()
	{
	int iter = 0;
	while ( RemoveUnused(++iter) )
		{
		body = rc->Reduce(body);
		Analyze();

		if ( reporter->Errors() > 0 )
			break;
		}
	}

void UseDefs::Dump()
	{
	for ( int i = stmts.size(); --i >= 0; )
		{
		auto& s = stmts[i];
		auto uds = FindUsage(s);
		auto are_copies =
			(UDs_are_copies.find(s) != UDs_are_copies.end());

		printf("UDs (%s) for %s:\n", are_copies ? "copy" : "orig",
			obj_desc(s));

		if ( uds )
			uds->Dump();
		else
			printf(" <none>");

		printf("\n\n");
		}
	}

bool UseDefs::RemoveUnused(int iter)
	{
	rc->ResetAlteredStmts();

	bool did_omission = false;

	for ( unsigned int i = 0; i < stmts.size(); ++i )
		{
		auto& s = stmts[i];

		if ( s->Tag() == STMT_INIT )
			{
			auto init = s->AsInitStmt();
			auto inits = init->Inits();
			auto used_ids = new id_list;
			for ( auto id : *inits )
				if ( is_atomic_type(id->Type()) ||
				     ! CheckIfUnused(s, id, false) )
					used_ids->append(id);

			if ( used_ids->length() < inits->length() )
				{
				if ( used_ids->length() == 0 )
					{
					rc->AddStmtToOmit(s);
					continue;
					}

				for ( auto id : *used_ids )
					Ref(id);

				auto new_init = new InitStmt(used_ids);
				rc->AddStmtToReplace(s, new_init);
				}

			else
				delete used_ids;

			continue;
			}

		if ( s->Tag() != STMT_EXPR )
			continue;

		auto s_e = s->AsExprStmt();
		auto e = s_e->StmtExpr();

		if ( e->Tag() != EXPR_ASSIGN )
			continue;

		auto a = e->AsAssignExpr();
		auto r = a->GetOp1();
		if ( r->Tag() != EXPR_REF )
			reporter->InternalError("lhs ref inconsistency in UseDefs::FindUnused");

		auto n = r->AsRefExpr()->GetOp1();
		if ( n->Tag() != EXPR_NAME )
			reporter->InternalError("lhs name inconsistency in UseDefs::FindUnused");

		auto id = n->AsNameExpr()->Id();

		auto rhs = a->GetOp2();
		auto rt = rhs->Tag();
		if ( rt == EXPR_CALL && ! rhs->IsPure() )
			// Need to do the call for the side effects.
			// Could prune out the assignment and just
			// keep the call, but not clear that's
			// worth the complexity.
			continue;

		if ( rt == EXPR_EVENT || rt == EXPR_SCHEDULE )
			// These always have side effects.
			continue;

		// Check for degenerate assignment.
		bool degen = rt == EXPR_NAME && id == rhs->AsNameExpr()->Id();

		if ( CheckIfUnused(s, id, iter == 1) || degen )
			{
			rc->AddStmtToOmit(s);
			did_omission = true;
			}
		}

	return did_omission;
	}

bool UseDefs::CheckIfUnused(const Stmt* s, const ID* id, bool report)
	{
	if ( id->IsGlobal() )
		return false;

	auto uds = FindSuccUsage(s);
	if ( ! uds || ! uds->HasID(id) )
		{
		if ( report && analysis_options.usage_issues > 0 &&
		     ! rc->IsTemporary(id) && ! rc->IsConstantVar(id) &&
		     ! rc->IsNewLocal(id) && ! id->FindAttr(ATTR_IS_USED) )
			reporter->Warning("%s assignment unused: %s",
						id->Name(), obj_desc(s));

		return true;
		}

	return false;
	}

UDs UseDefs::PropagateUDs(const Stmt* s, UDs succ_UDs, const Stmt* succ_stmt,
				bool second_pass)
	{
	if ( ! second_pass )
		stmts.push_back(s);

	switch ( s->Tag() ) {
	case STMT_LIST:
		{
		auto sl = s->AsStmtList();
		auto stmts = sl->Stmts();

		for ( int i = stmts.length(); --i >= 0; )
			{
			auto s_i = stmts[i];

			const Stmt* succ;

			if ( i == stmts.length() - 1 )
				{ // Very last statement.
				succ = succ_stmt;
				if ( successor2.find(s) != successor2.end() )
					successor2[s_i] = successor2[s];
				}
			else
				succ = stmts[i + 1];

			succ_UDs = PropagateUDs(s_i, succ_UDs, succ, second_pass);
			}

		return UseUDs(s, succ_UDs);
		}

	case STMT_CATCH_RETURN:
		{
                auto cr = s->AsCatchReturnStmt();
                auto block = cr->Block();

		auto uds = PropagateUDs(block.get(), succ_UDs, succ_stmt,
					second_pass);

		return UseUDs(s, uds);
		}

	case STMT_NULL:
	case STMT_NEXT:
	case STMT_BREAK:
	case STMT_FALLTHROUGH:
		// ### For most of these this isn't right, but Oh Well,
		// doesn't actually do any harm.  Also, we don't note
		// their successor.
		return UseUDs(s, succ_UDs);

	case STMT_PRINT:
		return CreateExprUDs(s, s->AsPrintStmt()->ExprList(), succ_UDs);

	case STMT_EVENT:
	case STMT_CHECK_ANY_LEN:
	case STMT_ADD:
	case STMT_DELETE:
	case STMT_RETURN:
		{
		auto e = ((const ExprStmt*) s)->StmtExpr();

		if ( e )
			return CreateExprUDs(s, e, succ_UDs);
		else
			return UseUDs(s, succ_UDs);
		}

	case STMT_EXPR:
		{
		auto e = s->AsExprStmt()->StmtExpr();

		if ( e->Tag() != EXPR_ASSIGN )
			return CreateExprUDs(s, e, succ_UDs);

		auto a = e->AsAssignExpr();
		auto lhs_ref = a->GetOp1();

		if ( lhs_ref->Tag() != EXPR_REF )
			reporter->InternalError("lhs inconsistency in UseDefs::ExprUDs");

		auto lhs_var = lhs_ref->GetOp1();
		auto lhs_id = lhs_var->AsNameExpr()->Id();
		auto lhs_UDs = RemoveID(lhs_id, succ_UDs);
		auto rhs_UDs = ExprUDs(a->GetOp2().get());
		auto uds = UD_Union(lhs_UDs, rhs_UDs);

		if ( ! second_pass )
			successor[s] = succ_stmt;

		return CreateUDs(s, uds);
		}

	case STMT_IF:
		{
		auto i = s->AsIfStmt();
		auto cond = i->StmtExpr();

		auto cond_UDs = ExprUDs(cond);
		auto true_UDs = PropagateUDs(i->TrueBranch(), succ_UDs,
						succ_stmt, second_pass);
		auto false_UDs = PropagateUDs(i->FalseBranch(), succ_UDs,
						succ_stmt, second_pass);

		auto uds = CreateUDs(s, UD_Union(cond_UDs, true_UDs, false_UDs));

		return uds;
		}

	case STMT_INIT:
		if ( ! second_pass )
			successor[s] = succ_stmt;

		return UseUDs(s, succ_UDs);

	case STMT_WHEN:
		// ###
		return UseUDs(s, succ_UDs);

	case STMT_SWITCH:
		{
		auto sw_UDs = make_intrusive<UseDefSet>();

		auto sw = s->AsSwitchStmt();
		auto cases = sw->Cases();

		for ( const auto& c : *cases )
			{
			auto body = c->Body();
			auto uds = PropagateUDs(body, succ_UDs, succ_stmt,
						second_pass);

			auto exprs = c->ExprCases();
			if ( exprs )
				{
				auto e_UDs = ExprUDs(exprs);
				uds = UD_Union(uds, e_UDs);
				}

			auto type_ids = c->TypeCases();
			if ( type_ids )
				for ( const auto& id : *type_ids )
					uds = RemoveID(id, uds);

			FoldInUDs(sw_UDs, uds);
			}

		auto e_UDs = ExprUDs(sw->StmtExpr());

		if ( sw->HasDefault() )
			FoldInUDs(sw_UDs, e_UDs);
		else
			// keep successor definitions in the mix
			FoldInUDs(sw_UDs, succ_UDs, e_UDs);

		return CreateUDs(s, sw_UDs);
		}

	case STMT_FOR:
		{
		auto f = s->AsForStmt();
		auto body = f->LoopBody();

		// The loop body has two potential successors, itself
		// and the successor of the entire "for" statement.
		successor2[body] = succ_stmt;
		auto body_UDs = PropagateUDs(body, succ_UDs, body, second_pass);

		auto e = f->LoopExpr();
		auto f_UDs = ExprUDs(e);
		FoldInUDs(f_UDs, body_UDs);

		// Confluence: loop the top UDs back around to the bottom.
		auto bottom_UDs = UD_Union(f_UDs, succ_UDs);
		(void) PropagateUDs(body, bottom_UDs, body, true);

		auto ids = f->LoopVars();
		for ( auto& id : *ids )
			RemoveUDFrom(f_UDs, id);

		auto val_var = f->ValueVar();
		if ( val_var )
			RemoveUDFrom(f_UDs, val_var);

		// The loop might not execute at all.
		FoldInUDs(f_UDs, succ_UDs);

		return CreateUDs(s, f_UDs);
		}

	case STMT_WHILE:
		{
		auto w = s->AsWhileStmt();
		auto body = w->Body();
		auto cond_stmt = w->CondStmt();

		// See note above for STMT_FOR regarding propagating
		// around the loop.
		auto succ = cond_stmt ? cond_stmt : body;
		successor2[body] = succ_stmt;
		auto body_UDs = PropagateUDs(body, succ_UDs, succ, second_pass);

		auto cond = w->Condition();
		auto w_UDs = UD_Union(ExprUDs(cond), body_UDs);
		FoldInUDs(w_UDs, body_UDs);

		if ( cond_stmt )
			{
			// Create a successor for the cond_stmt
			// that has the correct UDs associated with it.
			auto c_as_s = w->ConditionAsStmt();
			auto c_as_s_UDs = make_intrusive<UseDefSet>(w_UDs);
			CreateUDs(c_as_s, c_as_s_UDs);

			w_UDs = PropagateUDs(cond_stmt, w_UDs, c_as_s,
						second_pass);
			}

		// Confluence: loop the top UDs back around to the bottom.
		auto bottom_UDs = UD_Union(w_UDs, succ_UDs);
		(void) PropagateUDs(body, bottom_UDs, succ, true);

		// The loop might not execute at all.
		FoldInUDs(w_UDs, succ_UDs);

		return CreateUDs(s, w_UDs);
		}

	default:
		reporter->InternalError("non-reduced statement in use-def analysis");
	}
	}

UDs UseDefs::FindUsage(const Stmt* s) const
	{
	auto s_map = use_defs_map.find(s);

	if ( s_map == use_defs_map.end() )
		reporter->InternalError("missing use-defs");

	return s_map->second;
	}

UDs UseDefs::FindSuccUsage(const Stmt* s) const
	{
	auto succ = successor.find(s);
	auto no_succ = (succ == successor.end() || ! succ->second);
	auto uds = no_succ ? nullptr : FindUsage(succ->second);

	auto succ2 = successor2.find(s);
	auto no_succ2 = (succ2 == successor.end() || ! succ2->second);
	auto uds2 = no_succ2 ? nullptr : FindUsage(succ2->second);

	if ( uds && uds2 )
		return UD_Union(uds, uds2);

	else if ( uds )
		return uds;

	else
		return uds2;
	}

UDs UseDefs::ExprUDs(const Expr* e)
	{
	auto uds = make_intrusive<UseDefSet>();

	switch ( e->Tag() ) {
	case EXPR_NAME:
		AddInExprUDs(uds, e);
		break;

	case EXPR_FIELD_LHS_ASSIGN:
		{
		AddInExprUDs(uds, e->GetOp1().get());
		auto rhs_UDs = ExprUDs(e->GetOp2().get());
		uds = UD_Union(uds, rhs_UDs);
		break;
		}

	case EXPR_INCR:
	case EXPR_DECR:
		AddInExprUDs(uds, e->GetOp1()->AsRefExpr()->GetOp1().get());
		break;

	case EXPR_CONST:
		break;

	case EXPR_LAMBDA:
		{
		auto l = dynamic_cast<const LambdaExpr*>(e);
		auto ids = l->OuterIDs();

		for ( auto& id : ids )
			AddID(uds, id);
		break;
		}

	case EXPR_CALL:
		{
		auto c = e->AsCallExpr();
		AddInExprUDs(uds, c->Func());
		AddInExprUDs(uds, c->Args());
		break;
		}

	case EXPR_LIST:
		{
		auto l = e->AsListExpr();
		for ( const auto& l_e : l->Exprs() )
			AddInExprUDs(uds, l_e);

		break;
		}

	default:
		auto op1 = e->GetOp1();
		auto op2 = e->GetOp2();
		auto op3 = e->GetOp3();

		if ( ! op1 )
			reporter->InternalError("expression inconsistency in UseDefs::ExprUDs");

		AddInExprUDs(uds, op1.get());
		if ( op2 ) AddInExprUDs(uds, op2.get());
		if ( op3 ) AddInExprUDs(uds, op3.get());

		break;
	}

	return uds;
	}

void UseDefs::AddInExprUDs(UDs uds, const Expr* e)
	{
	if ( e->Tag() == EXPR_NAME )
		AddID(uds, e->AsNameExpr()->Id());

	else if ( e->Tag() == EXPR_LIST )
		{
		auto l = e->AsListExpr();
		for ( const auto& l_e : l->Exprs() )
			AddInExprUDs(uds, l_e);
		}

	else if ( e->Tag() == EXPR_EVENT )
		AddInExprUDs(uds, e->GetOp1().get());

	else if ( e->Tag() == EXPR_INCR || e->Tag() == EXPR_DECR )
		AddInExprUDs(uds, e->GetOp1()->AsRefExpr()->GetOp1().get());

	else if ( e->Tag() == EXPR_ASSIGN )
		{
		// These occur inside of table constructors.
		AddInExprUDs(uds, e->GetOp1().get());
		AddInExprUDs(uds, e->GetOp2().get());
		}

	else if ( e->Tag() == EXPR_FIELD_ASSIGN )
		{
		auto f = e->AsFieldAssignExpr();
		AddInExprUDs(uds, f->Op());
		}

	else if ( e->Tag() == EXPR_FIELD )
		{
		// This happens for append-to-field.
		auto f = e->AsFieldExpr();
		AddInExprUDs(uds, f->Op());
		}

	else if ( e->Tag() != EXPR_CONST )
		reporter->InternalError("bad tag in UseDefs::AddInExprUDs");
	}

void UseDefs::AddID(UDs uds, const ID* id) const
	{
	uds->Add(id);
	}

UDs UseDefs::RemoveID(const ID* id, const UDs& uds)
	{
	if ( ! uds )
		return nullptr;

	UDs new_uds = make_intrusive<UseDefSet>();

	new_uds->Replicate(uds);
	new_uds->Remove(id);

	return new_uds;
	}

void UseDefs::RemoveUDFrom(UDs uds, const ID* id)
	{
	if ( uds )
		uds->Remove(id);
	}

void UseDefs::FoldInUDs(UDs& main_UDs, const UDs& u1, const UDs& u2)
	{
	auto old_main = main_UDs;
	main_UDs = make_intrusive<UseDefSet>();

	if ( old_main )
		main_UDs->Replicate(old_main);

	if ( u1 )
		for ( auto ud : u1->IterateOver() )
			main_UDs->Add(ud);

	if ( u2 )
		for ( auto ud : u2->IterateOver() )
			main_UDs->Add(ud);
	}

void UseDefs::UpdateUDs(const Stmt* s, const UDs& uds)
	{
	auto curr_uds = FindUsage(s);

	if ( ! curr_uds || UDs_are_copies.find(s) != UDs_are_copies.end() )
		{
		// Copy-on-write.
		auto new_uds = make_intrusive<UseDefSet>();

		if ( curr_uds )
			new_uds->Replicate(curr_uds);

		CreateUDs(s, new_uds);

		curr_uds = new_uds;
		}

	if ( uds )
		{
		for ( auto u : uds->IterateOver() )
			curr_uds->Add(u);
		}
	}

UDs UseDefs::UD_Union(const UDs& u1, const UDs& u2, const UDs& u3) const
	{
	auto new_uds = make_intrusive<UseDefSet>();

	if ( u1 )
		new_uds->Replicate(u1);

	if ( u2 )
		for ( auto& u : u2->IterateOver() )
			AddID(new_uds, u);

	if ( u3 )
		for ( auto& u : u3->IterateOver() )
			AddID(new_uds, u);

	return new_uds;
	}

UDs UseDefs::UseUDs(const Stmt* s, UDs uds)
	{
	// printf("copying UDs %x for %x\n", uds, s);
	use_defs_map[s] = uds;
	UDs_are_copies.insert(s);
	return uds;
	}

UDs UseDefs::CreateExprUDs(const Stmt* s, const Expr* e, const UDs& uds)
	{
	auto e_UDs = ExprUDs(e);
	auto new_UDs = UD_Union(uds, e_UDs);

	return CreateUDs(s, new_UDs);
	}

UDs UseDefs::CreateUDs(const Stmt* s, UDs uds)
	{
	// printf("creating UDs %x for %x\n", uds, s);
	use_defs_map[s] = uds;
	UDs_are_copies.erase(s);
	return uds;
	}
