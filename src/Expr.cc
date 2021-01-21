// See the file "COPYING" in the main distribution directory for copyright.

#include "zeek-config.h"

#include "Expr.h"
#include "Event.h"
#include "Desc.h"
#include "Frame.h"
#include "Func.h"
#include "RE.h"
#include "Reduce.h"
#include "Inline.h"
#include "Compile.h"
#include "Scope.h"
#include "Stmt.h"
#include "EventRegistry.h"
#include "ScriptAnaly.h"
#include "Net.h"
#include "Traverse.h"
#include "Trigger.h"
#include "IPAddr.h"
#include "ZVal.h"
#include "digest.h"
#include "module_util.h"
#include "DebugLogger.h"

#include "broker/Data.h"

static int get_slice_index(int idx, int len)
	{
	if ( abs(idx) > len )
		idx = idx > 0 ? len : 0; // Clamp maximum positive/negative indices.
	else if ( idx < 0 )
		idx += len;  // Map to a positive index.

	return idx;
	}

const char* expr_name(BroExprTag t, bool is_describe)
	{
	static const char* expr_names[int(NUM_EXPRS)] = {
		"name", "const",
		"(*)",
		"++", "--", "!", "~", "+", "-",
		"+", "-", "+=", "vec+=", "-=", "*", "/", "%",
		"&", "|", "^",
		"&&", "||",
		"<", "<=", "==", "!=", ">=", ">", "?:", "ref",
		"=", "[]=", "$=", "[]", "any[]", "$", "?$", "[=]",
		"table()", "set()", "vector()",
		"$=", "in", "<<>>",
		"()", "inline()", "function()", "event", "schedule",
		"coerce", "record_coerce", "table_coerce", "vector_coerce",
		"to_any_coerce", "from_any_coerce",
		"sizeof", "cast", "is", "[:]=",
		"nop",
	};

	if ( int(t) >= NUM_EXPRS )
		{
		static char errbuf[512];

		// This isn't quite right - we return a static buffer,
		// so multiple calls to expr_name() could lead to confusion
		// by overwriting the buffer.  But oh well.
		snprintf(errbuf, sizeof(errbuf),
				"%d: not an expression tag", int(t));
		return errbuf;
		}

	if ( is_describe )
		switch ( t ) {
		case EXPR_TO_ANY_COERCE:
		case EXPR_FROM_ANY_COERCE:
			return "";

		default:
			break;
		}

	return expr_names[int(t)];
	}

Expr::Expr(BroExprTag arg_tag) : tag(arg_tag), type(0), paren(false)
	{
	original = nullptr;
	SetLocationInfo(&start_location, &end_location);
	}

bool Expr::CanAdd() const
	{
	return false;
	}

bool Expr::CanDel() const
	{
	return false;
	}

void Expr::Add(Frame* /* f */)
	{
	Internal("Expr::Add called");
	}

void Expr::Delete(Frame* /* f */)
	{
	Internal("Expr::Delete called");
	}

IntrusivePtr<Expr> Expr::MakeLvalue()
	{
	if ( ! IsError() )
		ExprError("can't be assigned to");

	return {NewRef{}, this};
	}

void Expr::EvalIntoAggregate(const BroType* /* t */, Val* /* aggr */,
				Frame* /* f */) const
	{
	Internal("Expr::EvalIntoAggregate called");
	}

void Expr::Assign(Frame* /* f */, IntrusivePtr<Val> /* v */)
	{
	Internal("Expr::Assign called");
	}

void Expr::AssignToIndex(IntrusivePtr<Val> v1, IntrusivePtr<Val> v2,
				IntrusivePtr<Val> v3) const
	{
	auto error_msg = assign_to_index(v1, v2, v3);

	if ( error_msg )
		RuntimeErrorWithCallStack(error_msg);
	}

const char* assign_to_index(IntrusivePtr<Val> v1, IntrusivePtr<Val> v2,
				IntrusivePtr<Val> v3)
	{
	if ( ! v1 || ! v2 || ! v3 )
		return nullptr;

	// Hold an extra reference to 'arg_v' in case the ownership transfer
	// to the table/vector goes wrong and we still want to obtain
	// diagnostic info from the original value after the assignment
	// already unref'd.
	auto v_extra = v3;

	switch ( v1->Type()->Tag() ) {
	case TYPE_VECTOR:
		{
		const ListVal* lv = v2->AsListVal();
		VectorVal* v1_vect = v1->AsVectorVal();

		if ( lv->Length() > 1 )
			{
			auto len = v1_vect->Size();
			bro_int_t first = get_slice_index(lv->Index(0)->CoerceToInt(), len);
			bro_int_t last = get_slice_index(lv->Index(1)->CoerceToInt(), len);

			// Remove the elements from the vector within the slice.
			for ( auto idx = first; idx < last; idx++ )
				v1_vect->Remove(first);

			// Insert the new elements starting at the first
			// position.

			VectorVal* v_vect = v3->AsVectorVal();

			for ( auto idx = 0u; idx < v_vect->Size();
			      idx++, first++ )
				v1_vect->Insert(first,
					v_vect->Lookup(idx)->Ref());
			}

		else if ( ! v1_vect->Assign(v2.get(), std::move(v3)) )
			{
			v3 = std::move(v_extra);

			if ( v3 )
				{
				ODesc d;
				v3->Describe(&d);
				auto vt = v3->Type();
				auto vtt = vt->Tag();
				std::string tn = vtt == TYPE_RECORD ?
					vt->GetName() : type_name(vtt);
				return fmt("vector index assignment failed for invalid type '%s', value: %s",
					tn.data(), d.Description());
				}
			else
				return "assignment failed with null value";
			}
		break;
		}

	case TYPE_TABLE:
		if ( ! v1->AsTableVal()->Assign(v2.get(), std::move(v3)) )
			{
			v3 = std::move(v_extra);

			if ( v3 )
				{
				ODesc d;
				v3->Describe(&d);
				auto vt = v3->Type();
				auto vtt = vt->Tag();
				std::string tn = vtt == TYPE_RECORD ?
					vt->GetName() : type_name(vtt);
				return fmt("table index assignment failed for invalid type '%s', value: %s",
					tn.data(), d.Description());
				}
			else
				return "assignment failed with null value";
			}
		break;

	case TYPE_STRING:
		return "assignment via string index accessor not allowed";
		break;

	default:
		return "bad index expression type in assignment";
		break;
	}

	return nullptr;
	}

IntrusivePtr<BroType> Expr::InitType() const
	{
	return type;
	}

bool Expr::IsRecordElement(TypeDecl* /* td */) const
	{
	return false;
	}

bool Expr::IsPure() const
	{
	return true;
	}

bool Expr::IsReduced(Reducer* c) const
	{
	return true;
	}

bool Expr::HasReducedOps(Reducer* c) const
	{
	return true;
	}

bool Expr::IsReducedConditional(Reducer* c) const
	{
	switch ( tag ) {
	case EXPR_CONST:
		return true;

	case EXPR_NAME:
		return IsReduced(c);

	case EXPR_IN:
		{
		auto op1 = GetOp1();
		auto op2 = GetOp2();

		if ( op1->Tag() != EXPR_NAME && op1->Tag() != EXPR_LIST )
			return NonReduced(this);

		if ( op2->Type()->Tag() != TYPE_TABLE || ! op2->IsReduced(c) )
			return NonReduced(this);

		if ( op1->Tag() == EXPR_LIST )
			{
			auto l1 = op1->AsListExpr();
			auto& l1_e = l1->Exprs();

			if ( l1_e.length() < 1 || l1_e.length() > 2 )
				return NonReduced(this);
			}

		return true;
		}

	case EXPR_EQ:
	case EXPR_NE:
	case EXPR_LE:
	case EXPR_GE:
	case EXPR_LT:
	case EXPR_GT:
	case EXPR_HAS_FIELD:
		return HasReducedOps(c);

	default:
		return false;
	}
	}

bool Expr::IsReducedFieldAssignment(Reducer* c) const
	{
	if ( ! IsFieldAssignable(this) )
		return false;

	if ( tag == EXPR_CONST )
		return true;

	if ( tag == EXPR_NAME )
		return IsReduced(c);

	return HasReducedOps(c);
	}

bool Expr::IsFieldAssignable(const Expr* e) const
	{
	switch ( e->Tag() ) {
	case EXPR_NAME:
	case EXPR_CONST:
	case EXPR_NOT:
	case EXPR_COMPLEMENT:
	case EXPR_POSITIVE:
	case EXPR_NEGATE:
	case EXPR_ADD:
	case EXPR_SUB:
	case EXPR_TIMES:
	case EXPR_DIVIDE:
	case EXPR_MOD:
	case EXPR_AND:
	case EXPR_OR:
	case EXPR_XOR:
	case EXPR_FIELD:
	case EXPR_HAS_FIELD:
	case EXPR_IN:
	case EXPR_SIZE:
		return true;

	// These would not be hard to add in principle, but at the expense
	// of some added complexity in the templator.  Seems unlikely the
	// actual performance gain would make that worth it.
	// case EXPR_LT:
	// case EXPR_LE:
	// case EXPR_EQ:
	// case EXPR_NE:
	// case EXPR_GE:
	// case EXPR_GT:

	// These could be added if we subsetted them to versions for
	// which we know it's safe to evaluate both operands.  Again
	// likely not worth it.
	// case EXPR_AND_AND:
	// case EXPR_OR_OR:

	default:
		return false;
	}
	}

Expr* Expr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	red_stmt = nullptr;
	return this->Ref();
	}

IntrusivePtr<Stmt> Expr::ReduceToSingletons(Reducer* c)
	{
	auto op1 = GetOp1();
	auto op2 = GetOp2();
	auto op3 = GetOp3();

	IntrusivePtr<Stmt> red1_stmt;
	IntrusivePtr<Stmt> red2_stmt;
	IntrusivePtr<Stmt> red3_stmt;

	if ( op1 && ! op1->IsSingleton(c) )
		SetOp1({AdoptRef{}, op1->ReduceToSingleton(c, red1_stmt)});
	if ( op2 && ! op2->IsSingleton(c) )
		SetOp2({AdoptRef{}, op2->ReduceToSingleton(c, red2_stmt)});
	if ( op3 && ! op3->IsSingleton(c) )
		SetOp3({AdoptRef{}, op3->ReduceToSingleton(c, red3_stmt)});

	return MergeStmts(red1_stmt, red2_stmt, red3_stmt);
	}

Expr* Expr::ReduceToConditional(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	switch ( tag ) {
	case EXPR_CONST:
		return this->Ref();

	case EXPR_NAME:
		if ( c->Optimizing() )
			return this->Ref();

		return Reduce(c, red_stmt);

	case EXPR_IN:
		{
		// This is complicated because there are lots of forms
		// of "in" expressions, and we're only interested in
		// those with 1 or 2 indices, into a table.
		auto op1 = GetOp1();
		auto op2 = GetOp2();

		if ( c->Optimizing() )
			return Reduce(c, red_stmt);

		if ( op2->Type()->Tag() != TYPE_TABLE )
			// Not a table de-reference.
			return Reduce(c, red_stmt);

		if ( op1->Tag() == EXPR_LIST )
			{
			auto l1 = op1->AsListExpr();
			auto& l1_e = l1->Exprs();

			if ( l1_e.length() < 1 || l1_e.length() > 2 )
				// Wrong number of indices.
				return Reduce(c, red_stmt);
			}

		if ( ! op1->IsReduced(c) || ! op2->IsReduced(c) )
			{
			auto red2_stmt = ReduceToSingletons(c);
			auto res = ReduceToConditional(c, red_stmt);
			red_stmt = MergeStmts(red2_stmt, red_stmt);
			return res;
			}

		return this->Ref();
		}

	case EXPR_EQ:
	case EXPR_NE:
	case EXPR_LE:
	case EXPR_GE:
	case EXPR_LT:
	case EXPR_GT:
		red_stmt = ReduceToSingletons(c);

		if ( GetOp1()->IsConst() && GetOp2()->IsConst() )
			// Fold!
			{
			IntrusivePtr<Stmt> fold_stmts;
			auto new_me = Reduce(c, fold_stmts);
			red_stmt = MergeStmts(red_stmt, fold_stmts);

			return new_me;
			}

		return this->Ref();

	case EXPR_HAS_FIELD:
		red_stmt = ReduceToSingletons(c);
		return this->Ref();

	default:
		return Reduce(c, red_stmt);
	}
	}

Expr* Expr::ReduceToFieldAssignment(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( ! IsFieldAssignable(this) || tag == EXPR_NAME )
		return ReduceToSingleton(c, red_stmt);

	red_stmt = ReduceToSingletons(c);

	return this->Ref();
	}

const CompiledStmt Expr::Compile(Compiler* c) const
	{
	reporter->InternalError("confused in Expr::Compile");
	}

IntrusivePtr<Stmt> Expr::MergeStmts(IntrusivePtr<Stmt> s1,
					IntrusivePtr<Stmt> s2,
					IntrusivePtr<Stmt> s3) const
	{
	int nums = (s1 != nullptr) + (s2 != nullptr) + (s3 != nullptr);

	if ( nums > 1 )
		return make_intrusive<StmtList>(s1, s2, s3);
	else if ( s1 )
		return s1;
	else if ( s2 )
		return s2;
	else if ( s3 )
		return s3;
	else
		return nullptr;
	}

IntrusivePtr<Val> Expr::InitVal(const BroType* t, IntrusivePtr<Val> aggr) const
	{
	if ( aggr )
		{
		Error("bad initializer");
		return nullptr;
		}

	if ( IsError() )
		return nullptr;

	return check_and_promote(Eval(nullptr), t, true);
	}

void Expr::SeatBelts(const BroType* t1, const BroType* t2) const
	{
	if ( same_type(t1, t2) )
		return;

	auto tt1 = t1->Tag();
	auto tt2 = t2->Tag();

	if ( t1->Tag() == TYPE_ERROR || t2->Tag() == TYPE_ERROR )
		return;

	if ( t1->Tag() == TYPE_LIST || t2->Tag() == TYPE_LIST )
		// These arise from indexing "any" types.
		return;

	printf("type mismatch for %s\n", obj_desc(this));
	printf(" ... %s vs. ", obj_desc(t1));
	printf("%s\n", obj_desc(t2));
	reporter->InternalError("SeatBelts");
	}

Val* Expr::MakeZero(TypeTag t) const
	{
	switch ( t ) {
	case TYPE_BOOL:		return val_mgr->GetFalse();
	case TYPE_INT:		return val_mgr->GetInt(0);
	case TYPE_COUNT:	return val_mgr->GetCount(0);

	case TYPE_DOUBLE:	return new Val(0.0, TYPE_DOUBLE);
	case TYPE_TIME:		return new Val(0.0, TYPE_TIME);
	case TYPE_INTERVAL:	return new IntervalVal(0.0, 1.0);

	default:
		reporter->InternalError("bad call to MakeZero");
	}
	}

ConstExpr* Expr::MakeZeroExpr(TypeTag t) const
	{
	auto z = MakeZero(t);
	return new ConstExpr({AdoptRef{}, z});
	}

bool Expr::IsError() const
	{
	return type && type->Tag() == TYPE_ERROR;
	}

void Expr::SetError()
	{
	SetType(error_type());
	}

void Expr::SetError(const char* msg)
	{
	Error(msg);
	SetError();
	}

IntrusivePtr<Expr> Expr::GetOp1() const { return nullptr; }
IntrusivePtr<Expr> Expr::GetOp2() const { return nullptr; }
IntrusivePtr<Expr> Expr::GetOp3() const { return nullptr; }

void Expr::SetOp1(IntrusivePtr<Expr>) { }
void Expr::SetOp2(IntrusivePtr<Expr>) { }
void Expr::SetOp3(IntrusivePtr<Expr>) { }

bool Expr::IsZero() const
	{
	return IsConst() && ExprVal()->IsZero();
	}

bool Expr::IsOne() const
	{
	return IsConst() && ExprVal()->IsOne();
	}

void Expr::Describe(ODesc* d) const
	{
	if ( IsParen() && ! d->IsBinary() )
		d->Add("(");

	if ( d->IsBinary() )
		AddTag(d);

	if ( d->DoOrig() )
		Original()->ExprDescribe(d);
	else
		{
		// d->Add("{");
		ExprDescribe(d);

		// d->Add(" (type ");
		// d->Add(type_name(Type()->Tag()));
		// d->Add(")}");
		}

	if ( IsParen() && ! d->IsBinary() )
		d->Add(")");
	}

void Expr::AddTag(ODesc* d) const
	{
	if ( d->IsBinary() )
		d->Add(int(Tag()));
	else
		d->AddSP(expr_name(Tag()));
	}

void Expr::Canonicize()
	{
	}

Expr* Expr::AssignToTemporary(Expr* e, Reducer* c,
				IntrusivePtr<Stmt>& red_stmt)
	{
	IntrusivePtr<Expr> e_ptr = {NewRef{}, e};
	auto result_tmp = c->GenTemporaryExpr(Type(), e_ptr);

	auto a_e = get_temp_assign_expr(result_tmp->MakeLvalue(), e_ptr);
	if ( a_e->Tag() != EXPR_ASSIGN )
		Internal("confusion in AssignToTemporary");

	a_e->AsAssignExpr()->SetIsTemp();
	a_e->SetOriginal(this);

	IntrusivePtr<Stmt> a_e_s = {AdoptRef{}, new ExprStmt(a_e)};
	red_stmt = MergeStmts(red_stmt, a_e_s);

	// Important: our result is not result_tmp, but a duplicate of it.
	// This is important because subsequent passes that associate
	// information with Expr's need to not mis-associate that
	// information with both the assignment creating the temporary,
	// and the subsequent use of the temporary.
	return result_tmp->Duplicate().release();
	}

Expr* Expr::TransformMe(Expr* new_me, Reducer* c,
			IntrusivePtr<Stmt>& red_stmt)
	{
	if ( new_me == this )
		return this;

	new_me->SetOriginal(this);

	// Unlike for Stmt's, we assume that new_me has already
	// been reduced, so no need to do so further.
	return new_me;
	}

void Expr::SetType(IntrusivePtr<BroType> t)
	{
	if ( ! type || type->Tag() != TYPE_ERROR )
		type = std::move(t);
	}

void Expr::ExprError(const char msg[])
	{
	Error(msg);
	SetError();
	}

void Expr::RuntimeError(const std::string& msg) const
	{
	reporter->ExprRuntimeError(Original(), "%s", msg.data());
	}

void Expr::RuntimeErrorWithCallStack(const std::string& msg) const
	{
	auto rcs = render_call_stack();

	if ( rcs.empty() )
		reporter->ExprRuntimeError(Original(), "%s", msg.data());
	else
		{
		ODesc d;
		d.SetShort();
		Describe(&d);
		reporter->RuntimeError(Original()->GetLocationInfo(),
					"%s, expression: %s, call stack: %s",
		                       msg.data(), d.Description(), rcs.data());
		}
	}

NameExpr::NameExpr(IntrusivePtr<ID> arg_id, bool const_init)
	: Expr(EXPR_NAME), id(std::move(arg_id))
	{
	in_const_init = const_init;

	if ( id->AsType() )
		SetType(make_intrusive<TypeType>(IntrusivePtr{NewRef{}, id->AsType()}));
	else
		SetType({NewRef{}, id->Type()});

	EventHandler* h = event_registry->Lookup(id->Name());
	if ( h )
		h->SetUsed();
	}

IntrusivePtr<Val> NameExpr::FoldVal() const
	{
	if ( ! id->IsConst() || id->FindAttr(ATTR_REDEF) ||
	     id->Type()->Tag() == TYPE_FUNC )
		return nullptr;

	return {NewRef{}, id->ID_Val()};
	}

IntrusivePtr<Val> NameExpr::Eval(Frame* f) const
	{
	IntrusivePtr<Val> v;

	if ( id->AsType() )
		return make_intrusive<Val>(id->AsType(), true);

	if ( id->IsGlobal() )
		v = {NewRef{}, id->ID_Val()};

	else if ( f )
		v = {NewRef{}, f->GetElement(id.get())};

	else
		// No frame - evaluating for folding purposes
		return nullptr;

	if ( v )
		return v;
	else
		{
		RuntimeError("value used but not set");
		return nullptr;
		}
	}

IntrusivePtr<Expr> NameExpr::MakeLvalue()
	{
	if ( id->AsType() )
		ExprError("Type name is not an lvalue");

	if ( id->IsConst() && ! in_const_init )
		ExprError("const is not a modifiable lvalue");

	if ( id->IsOption() && ! in_const_init )
		ExprError("option is not a modifiable lvalue");

	return make_intrusive<RefExpr>(IntrusivePtr{NewRef{}, this});
	}

void NameExpr::Assign(Frame* f, IntrusivePtr<Val> v)
	{
	if ( id->IsGlobal() )
		id->SetVal(std::move(v));
	else
		f->SetElement(id.get(), v.release());
	}

bool NameExpr::IsPure() const
	{
	if ( ! id->IsConst() )
		return false;

	if ( id->Type()->Tag() != TYPE_FUNC )
		return true;

	if ( ! id->IsGlobal() )
		return false;

	auto v = id->ID_Val();
	if ( ! v )
		return false;

	auto f = v->AsFunc();

	if ( ! f->HasBodies() )
		// We don't know how it might change.
		return false;

	return f->IsPure();
	}

bool NameExpr::IsReduced(Reducer* c) const
	{
	if ( FoldableGlobal() )
		return false;

	return c->NameIsReduced(this);
	}

Expr* NameExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	red_stmt = nullptr;

	if ( c->Optimizing() )
		return this->Ref();

	if ( FoldableGlobal() )
		{
		IntrusivePtr<Val> v = {NewRef{}, id->ID_Val()};
		ASSERT(v);
		return TransformMe(new ConstExpr(v), c, red_stmt);
		}

	return c->UpdateName(this);
	}

IntrusivePtr<Expr> NameExpr::Duplicate()
	{
	// We need to create a replicate because Reaching Defs for different
	// instances of the name need to be kept distinct, and these are
	// done based on the pointer to the NameExpr.
	return SetSucc(new NameExpr(id, in_const_init));
	}

bool NameExpr::FoldableGlobal() const
	{
	return id->IsGlobal() && id->IsConst() && is_atomic_type(id->Type()) &&
		// Make sure constant can't be changed on the command line
		// or such.
		! id->FindAttr(ATTR_REDEF);
	}

TraversalCode NameExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = id->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

void NameExpr::ExprDescribe(ODesc* d) const
	{
	if ( d->IsReadable() )
		d->Add(id->Name());
	else
		{
		if ( d->IsParseable() )
			d->Add(id->Name());
		else
			d->AddCS(id->Name());
		}
	}

ConstExpr::ConstExpr(IntrusivePtr<Val> arg_val)
	: Expr(EXPR_CONST), val(std::move(arg_val))
	{
	if ( val )
		{
		if ( val->Type()->Tag() == TYPE_LIST &&
		     val->AsListVal()->Length() == 1 )
			val = {NewRef{}, val->AsListVal()->Index(0)};

		SetType({NewRef{}, val->Type()});
		}

	else
		SetError();
	}

void ConstExpr::ExprDescribe(ODesc* d) const
	{
	val->Describe(d);
	}

IntrusivePtr<Val> ConstExpr::Eval(Frame* /* f */) const
	{
	return {NewRef{}, Value()};
	}

TraversalCode ConstExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

UnaryExpr::UnaryExpr(BroExprTag arg_tag, IntrusivePtr<Expr> arg_op)
	: Expr(arg_tag), op(std::move(arg_op))
	{
	if ( op->IsError() )
		SetError();
	}

IntrusivePtr<Val> UnaryExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return nullptr;

	auto v = op->Eval(f);

	if ( ! v )
		return nullptr;

	auto t = Tag();

	// Check for operations that should be vectorized.
	if ( is_vector(v.get()) && t != EXPR_IS && t != EXPR_CAST &&
	     // The following allows passing vectors-by-reference to
	     // functions that use vector-of-any for generic vector
	     // manipulation ...
	     t != EXPR_TO_ANY_COERCE &&
	     // ... and the following to avoid vectorizing operations
	     // on vector-of-any's
	     t != EXPR_FROM_ANY_COERCE )
		{
		VectorVal* v_op = v->AsVectorVal();
		VectorType* out_t;
		if ( Type()->Tag() == TYPE_ANY )
			out_t = v->Type()->AsVectorType();
		else
			out_t = Type()->AsVectorType();

		auto result = make_intrusive<VectorVal>(out_t);

		for ( unsigned int i = 0; i < v_op->Size(); ++i )
			{
			auto v_i = v_op->Lookup(i);
			result->Assign(i, v_i ? Fold(v_i.get()) : nullptr);
			}

		return result;
		}
	else
		{
		return Fold(v.get());
		}
	}

bool UnaryExpr::IsPure() const
	{
	return op->IsPure();
	}

bool UnaryExpr::HasNoSideEffects() const
	{
	return op->HasNoSideEffects();
	}

bool UnaryExpr::IsReduced(Reducer* c) const
	{
	return NonReduced(this);
	}

bool UnaryExpr::HasReducedOps(Reducer* c) const
	{
	return op->IsSingleton(c);
	}

Expr* UnaryExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( c->Optimizing() )
		op = c->UpdateExpr(op);

	red_stmt = nullptr;

	if ( ! op->IsSingleton(c) )
		op = {AdoptRef{}, op->ReduceToSingleton(c, red_stmt)};

	auto op_val = op->FoldVal();
	if ( op_val )
		{
		auto fold = Fold(op_val.get());
		return TransformMe(new ConstExpr(fold), c, red_stmt);
		}

	if ( c->Optimizing() )
		return this->Ref();
	else
		return AssignToTemporary(c, red_stmt);
	}

Expr* UnaryExpr::Inline(Inliner* inl)
	{
	op = {NewRef{}, op->Inline(inl)};
	return this->Ref();
	}

TraversalCode UnaryExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this, Op());
	HANDLE_TC_EXPR_PRE(tc);

	tc = op->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

IntrusivePtr<Val> UnaryExpr::Fold(Val* v) const
	{
	return {NewRef{}, v};
	}

void UnaryExpr::ExprDescribe(ODesc* d) const
	{
	bool is_coerce =
		Tag() == EXPR_ARITH_COERCE || Tag() == EXPR_RECORD_COERCE ||
		Tag() == EXPR_TABLE_COERCE;
	bool explicit_refs = getenv("ZEEK_SHOW_REFS") != nullptr;

	if ( d->IsReadable() )
		{
		if ( is_coerce )
			d->Add("(coerce ");
		else
			{
			if ( Tag() == EXPR_REF )
				{
				if ( explicit_refs )
					{
					d->Add("(");
					d->Add(expr_name(Tag()));
					d->SP();
					}
				}
			else
				d->Add(expr_name(Tag(), true));
			}
		}

	op->Describe(d);

	if ( d->IsReadable() )
		{
		if ( is_coerce )
			{
			d->Add(" to ");
			Type()->Describe(d);
			d->Add(")");
			}

		else if ( Tag() == EXPR_REF && explicit_refs)
			d->Add(")");
		}
	}

IntrusivePtr<Val> BinaryExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return nullptr;

	auto v1 = op1->Eval(f);

	if ( ! v1 )
		return nullptr;

	auto v2 = op2->Eval(f);

	if ( ! v2 )
		return nullptr;

	bool is_vec1 = is_vector(v1.get());
	bool is_vec2 = is_vector(v2.get());

	if ( is_vec1 && is_vec2 )
		{ // fold pairs of elements
		VectorVal* v_op1 = v1->AsVectorVal();
		VectorVal* v_op2 = v2->AsVectorVal();

		if ( v_op1->Size() != v_op2->Size() )
			{
			RuntimeError("vector operands are of different sizes");
			return nullptr;
			}

		auto v_result = make_intrusive<VectorVal>(Type()->AsVectorType());

		for ( unsigned int i = 0; i < v_op1->Size(); ++i )
			{
			if ( v_op1->Lookup(i) && v_op2->Lookup(i) )
				{
				auto v1 = v_op1->Lookup(i);
				auto v2 = v_op2->Lookup(i);
				v_result->Assign(i, Fold(v1.get(), v2.get()));
				}
			else
				v_result->Assign(i, nullptr);
			// SetError("undefined element in vector operation");
			}

		return v_result;
		}

	if ( IsVector(Type()->Tag()) && (is_vec1 || is_vec2) )
		{ // fold vector against scalar
		VectorVal* vv = (is_vec1 ? v1 : v2)->AsVectorVal();
		auto v_result = make_intrusive<VectorVal>(Type()->AsVectorType());

		for ( unsigned int i = 0; i < vv->Size(); ++i )
			{
			if ( auto vv_i = vv->Lookup(i) )
				v_result->Assign(i, is_vec1 ? Fold(vv_i.get(), v2.get())
				                            : Fold(v1.get(), vv_i.get()));
			else
				v_result->Assign(i, nullptr);

			// SetError("Undefined element in vector operation");
			}

		return v_result;
		}

	// scalar op scalar
	return Fold(v1.get(), v2.get());
	}

bool BinaryExpr::IsPure() const
	{
	return op1->IsPure() && op2->IsPure();
	}

bool BinaryExpr::HasNoSideEffects() const
	{
	return op1->HasNoSideEffects() && op2->HasNoSideEffects();
	}

bool BinaryExpr::IsReduced(Reducer* c) const
	{
	return NonReduced(this);
	}

bool BinaryExpr::HasReducedOps(Reducer* c) const
	{
	return op1->IsSingleton(c) && op2->IsSingleton(c);
	}

Expr* BinaryExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( c->Optimizing() )
		{
		op1 = c->UpdateExpr(op1);
		op2 = c->UpdateExpr(op2);
		}

	red_stmt = nullptr;

	if ( ! op1->IsSingleton(c) )
		op1 = {AdoptRef{}, op1->ReduceToSingleton(c, red_stmt)};

	IntrusivePtr<Stmt> red2_stmt;
	if ( ! op2->IsSingleton(c) )
		op2 = {AdoptRef{}, op2->ReduceToSingleton(c, red2_stmt)};

	red_stmt = MergeStmts(red_stmt, red2_stmt);

	auto op1_fold_val = op1->FoldVal();
	auto op2_fold_val = op2->FoldVal();
	if ( op1_fold_val && op2_fold_val )
		{
		auto fold = Fold(op1_fold_val.get(), op2_fold_val.get());
		return TransformMe(new ConstExpr(fold), c, red_stmt);
		}

	if ( c->Optimizing() )
		return this->Ref();
	else
		return AssignToTemporary(c, red_stmt);
	}

Expr* BinaryExpr::Inline(Inliner* inl)
	{
	op1 = {NewRef{}, op1->Inline(inl)};
	op2 = {NewRef{}, op2->Inline(inl)};

	return this->Ref();
	}

TraversalCode BinaryExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this, Op1(), Op2());
	HANDLE_TC_EXPR_PRE(tc);

	tc = op1->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = op2->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

void BinaryExpr::ExprDescribe(ODesc* d) const
	{
	op1->Describe(d);

	d->SP();
	if ( d->IsReadable() )
		d->AddSP(expr_name(Tag()));

	op2->Describe(d);
	}

IntrusivePtr<Val> BinaryExpr::Fold(Val* v1, Val* v2) const
	{
	InternalTypeTag it = v1->Type()->InternalType();

	if ( it == TYPE_INTERNAL_STRING )
		return StringFold(v1, v2);

	if ( v1->Type()->Tag() == TYPE_PATTERN )
		return PatternFold(v1, v2);

	if ( v1->Type()->IsSet() )
		return SetFold(v1, v2);

	if ( it == TYPE_INTERNAL_ADDR )
		return AddrFold(v1, v2);

	if ( it == TYPE_INTERNAL_SUBNET )
		return SubNetFold(v1, v2);

	bro_int_t i1 = 0, i2 = 0, i3 = 0;
	bro_uint_t u1 = 0, u2 = 0, u3 = 0;
	double d1 = 0.0, d2 = 0.0, d3 = 0.0;
	bool is_integral = false;
	bool is_unsigned = false;

	if ( it == TYPE_INTERNAL_INT )
		{
		i1 = v1->InternalInt();
		i2 = v2->InternalInt();
		is_integral = true;
		}
	else if ( it == TYPE_INTERNAL_UNSIGNED )
		{
		u1 = v1->InternalUnsigned();
		u2 = v2->InternalUnsigned();
		is_unsigned = true;
		}
	else if ( it == TYPE_INTERNAL_DOUBLE )
		{
		d1 = v1->InternalDouble();
		d2 = v2->InternalDouble();
		}
	else
		RuntimeErrorWithCallStack("bad type in BinaryExpr::Fold");

	switch ( tag ) {
#define DO_INT_FOLD(op) \
	if ( is_integral ) \
		i3 = i1 op i2; \
	else if ( is_unsigned ) \
		u3 = u1 op u2; \
	else \
		RuntimeErrorWithCallStack("bad type in BinaryExpr::Fold");

#define DO_UINT_FOLD(op) \
	if ( is_unsigned ) \
		u3 = u1 op u2; \
	else \
		RuntimeErrorWithCallStack("bad type in BinaryExpr::Fold");

#define DO_FOLD(op) \
	if ( is_integral ) \
		i3 = i1 op i2; \
	else if ( is_unsigned ) \
		u3 = u1 op u2; \
	else \
		d3 = d1 op d2;

#define DO_INT_VAL_FOLD(op) \
	if ( is_integral ) \
		i3 = i1 op i2; \
	else if ( is_unsigned ) \
		i3 = u1 op u2; \
	else \
		i3 = d1 op d2;

	case EXPR_ADD:
	case EXPR_ADD_TO:	DO_FOLD(+); break;
	case EXPR_SUB:
	case EXPR_REMOVE_FROM:	DO_FOLD(-); break;
	case EXPR_TIMES:	DO_FOLD(*); break;
	case EXPR_DIVIDE:
		{
		if ( is_integral )
			{
			if ( i2 == 0 )
				RuntimeError("division by zero");

			i3 = i1 / i2;
			}

		else if ( is_unsigned )
			{
			if ( u2 == 0 )
				RuntimeError("division by zero");

			u3 = u1 / u2;
			}
		else
			{
			if ( d2 == 0 )
				RuntimeError("division by zero");

			d3 = d1 / d2;
			}

		}
		break;

	case EXPR_MOD:
		{
		if ( is_integral )
			{
			if ( i2 == 0 )
				RuntimeError("modulo by zero");

			i3 = i1 % i2;
			}

		else if ( is_unsigned )
			{
			if ( u2 == 0 )
				RuntimeError("modulo by zero");

			u3 = u1 % u2;
			}

		else
			RuntimeErrorWithCallStack("bad type in BinaryExpr::Fold");
		}

		break;

	case EXPR_AND:		DO_UINT_FOLD(&); break;
	case EXPR_OR:		DO_UINT_FOLD(|); break;
	case EXPR_XOR:		DO_UINT_FOLD(^); break;

	case EXPR_AND_AND:	DO_INT_FOLD(&&); break;
	case EXPR_OR_OR:	DO_INT_FOLD(||); break;

	case EXPR_LT:		DO_INT_VAL_FOLD(<); break;
	case EXPR_LE:		DO_INT_VAL_FOLD(<=); break;
	case EXPR_EQ:		DO_INT_VAL_FOLD(==); break;
	case EXPR_NE:		DO_INT_VAL_FOLD(!=); break;
	case EXPR_GE:		DO_INT_VAL_FOLD(>=); break;
	case EXPR_GT:		DO_INT_VAL_FOLD(>); break;

	default:
		BadTag("BinaryExpr::Fold", expr_name(tag));
	}

	auto ret_type = Type().get();

	if ( IsVector(ret_type->Tag()) )
	     ret_type = ret_type->YieldType();

	if ( ret_type->Tag() == TYPE_INTERVAL )
		return make_intrusive<IntervalVal>(d3, 1.0);
	else if ( ret_type->InternalType() == TYPE_INTERNAL_DOUBLE )
		return make_intrusive<Val>(d3, ret_type->Tag());
	else if ( ret_type->InternalType() == TYPE_INTERNAL_UNSIGNED )
		return {AdoptRef{}, val_mgr->GetCount(u3)};
	else if ( ret_type->Tag() == TYPE_BOOL )
		return {AdoptRef{}, val_mgr->GetBool(i3)};
	else
		return {AdoptRef{}, val_mgr->GetInt(i3)};
	}

IntrusivePtr<Val> BinaryExpr::StringFold(Val* v1, Val* v2) const
	{
	const BroString* s1 = v1->AsString();
	const BroString* s2 = v2->AsString();
	int result = 0;

	switch ( tag ) {
#undef DO_FOLD
#define DO_FOLD(sense) { result = Bstr_cmp(s1, s2) sense 0; break; }

	case EXPR_LT:		DO_FOLD(<)
	case EXPR_LE:		DO_FOLD(<=)
	case EXPR_EQ:		DO_FOLD(==)
	case EXPR_NE:		DO_FOLD(!=)
	case EXPR_GE:		DO_FOLD(>=)
	case EXPR_GT:		DO_FOLD(>)

	case EXPR_ADD:
	case EXPR_ADD_TO:
		{
		vector<const BroString*> strings;
		strings.push_back(s1);
		strings.push_back(s2);

		return make_intrusive<StringVal>(concatenate(strings));
		}

	default:
		BadTag("BinaryExpr::StringFold", expr_name(tag));
	}

	return {AdoptRef{}, val_mgr->GetBool(result)};
	}


IntrusivePtr<Val> BinaryExpr::PatternFold(Val* v1, Val* v2) const
	{
	const RE_Matcher* re1 = v1->AsPattern();
	const RE_Matcher* re2 = v2->AsPattern();

	if ( tag != EXPR_AND && tag != EXPR_OR )
		BadTag("BinaryExpr::PatternFold");

	RE_Matcher* res = tag == EXPR_AND ?
		RE_Matcher_conjunction(re1, re2) :
		RE_Matcher_disjunction(re1, re2);

	return make_intrusive<PatternVal>(res);
	}

IntrusivePtr<Val> BinaryExpr::SetFold(Val* v1, Val* v2) const
	{
	TableVal* tv1 = v1->AsTableVal();
	TableVal* tv2 = v2->AsTableVal();
	bool res = false;

	switch ( tag ) {
	case EXPR_AND:
		return {AdoptRef{}, tv1->Intersect(tv2)};

	case EXPR_OR:
		{
		auto rval = v1->Clone();

		if ( ! tv2->AddTo(rval.get(), false, false) )
			reporter->InternalError("set union failed to type check");

		return rval;
		}

	case EXPR_SUB:
		{
		auto rval = v1->Clone();

		if ( ! tv2->RemoveFrom(rval.get()) )
			reporter->InternalError("set difference failed to type check");

		return rval;
		}

	case EXPR_EQ:
		res = tv1->EqualTo(tv2);
		break;

	case EXPR_NE:
		res = ! tv1->EqualTo(tv2);
		break;

	case EXPR_LT:
		res = tv1->IsSubsetOf(tv2) && tv1->Size() < tv2->Size();
		break;

	case EXPR_LE:
		res = tv1->IsSubsetOf(tv2);
		break;

	case EXPR_GE:
	case EXPR_GT:
		// These should't happen due to canonicalization.
		reporter->InternalError("confusion over canonicalization in set comparison");
		break;

	default:
		BadTag("BinaryExpr::SetFold", expr_name(tag));
		return nullptr;
	}

	return {AdoptRef{}, val_mgr->GetBool(res)};
	}

IntrusivePtr<Val> BinaryExpr::AddrFold(Val* v1, Val* v2) const
	{
	IPAddr a1 = v1->AsAddr();
	IPAddr a2 = v2->AsAddr();
	bool result = false;

	switch ( tag ) {

	case EXPR_LT:
		result = a1 < a2;
		break;
	case EXPR_LE:
		result = a1 < a2 || a1 == a2;
		break;
	case EXPR_EQ:
		result = a1 == a2;
		break;
	case EXPR_NE:
		result = a1 != a2;
		break;
	case EXPR_GE:
		result = ! ( a1 < a2 );
		break;
	case EXPR_GT:
		result = ( ! ( a1 < a2 ) ) && ( a1 != a2 );
		break;

	default:
		BadTag("BinaryExpr::AddrFold", expr_name(tag));
	}

	return {AdoptRef{}, val_mgr->GetBool(result)};
	}

IntrusivePtr<Val> BinaryExpr::SubNetFold(Val* v1, Val* v2) const
	{
	const IPPrefix& n1 = v1->AsSubNet();
	const IPPrefix& n2 = v2->AsSubNet();

	bool result = n1 == n2;

	if ( tag == EXPR_NE )
		result = ! result;

	return {AdoptRef{}, val_mgr->GetBool(result)};
	}

void BinaryExpr::SwapOps()
	{
	// We could check here whether the operator is commutative.
	using std::swap;
	swap(op1, op2);
	}

void BinaryExpr::PromoteOps(TypeTag t)
	{
	TypeTag bt1 = op1->Type()->Tag();
	TypeTag bt2 = op2->Type()->Tag();

	bool is_vec1 = IsVector(bt1);
	bool is_vec2 = IsVector(bt2);

	if ( is_vec1 )
		bt1 = op1->Type()->AsVectorType()->YieldType()->Tag();
	if ( is_vec2 )
		bt2 = op2->Type()->AsVectorType()->YieldType()->Tag();

	if ( (is_vec1 || is_vec2) && ! (is_vec1 && is_vec2) )
		reporter->Warning("mixing vector and scalar operands is deprecated");

	if ( bt1 != t )
		op1 = make_intrusive<ArithCoerceExpr>(op1, t);
	if ( bt2 != t )
		op2 = make_intrusive<ArithCoerceExpr>(op2, t);
	}

void BinaryExpr::PromoteType(TypeTag t, bool is_vector)
	{
	PromoteOps(t);

	if ( is_vector)
		SetType(make_intrusive<VectorType>(base_type(t)));
	else
		SetType(base_type(t));
	}

void BinaryExpr::PromoteForInterval(IntrusivePtr<Expr>& op)
	{
	if ( is_vector(op1.get()) || is_vector(op2.get()) )
		SetType(make_intrusive<VectorType>(base_type(TYPE_INTERVAL)));
	else
		SetType(base_type(TYPE_INTERVAL));

	if ( op->Type()->Tag() != TYPE_DOUBLE )
		op = make_intrusive<ArithCoerceExpr>(op, TYPE_DOUBLE);
	}

CloneExpr::CloneExpr(IntrusivePtr<Expr> arg_op)
	: UnaryExpr(EXPR_CLONE, std::move(arg_op))
	{
	if ( IsError() )
		return;

	SetType(op->Type());
	}

IntrusivePtr<Val> CloneExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return nullptr;

	if ( auto v = op->Eval(f) )
		return Fold(v.get());

	return nullptr;
	}

IntrusivePtr<Val> CloneExpr::Fold(Val* v) const
	{
	return v->Clone();
	}

IntrusivePtr<Expr> CloneExpr::Duplicate()
	{
	// oh the irony
	return SetSucc(new CloneExpr(op->Duplicate()));
	}

IncrExpr::IncrExpr(BroExprTag arg_tag, IntrusivePtr<Expr> arg_op)
	: UnaryExpr(arg_tag, arg_op->MakeLvalue())
	{
	if ( IsError() )
		return;

	auto t = op->Type();

	if ( IsVector(t->Tag()) )
		{
		if ( ! IsIntegral(t->AsVectorType()->YieldType()->Tag()) )
			ExprError("vector elements must be integral for increment operator");
		else
			{
			reporter->Warning("increment/decrement operations for vectors deprecated");
			SetType(t);
			}
		}
	else
		{
		if ( ! IsIntegral(t->Tag()) )
			ExprError("requires an integral operand");
		else
			SetType(t);
		}
	}

IntrusivePtr<Val> IncrExpr::DoSingleEval(Frame* f, Val* v) const
	{
	bro_int_t k = v->CoerceToInt();

	if ( Tag() == EXPR_INCR )
		++k;
	else
		{
		--k;

		if ( k < 0 &&
		     v->Type()->InternalType() == TYPE_INTERNAL_UNSIGNED )
			RuntimeError("count underflow");
		}

	auto ret_type = Type().get();
	if ( IsVector(ret_type->Tag()) )
		ret_type = Type()->YieldType();

	if ( ret_type->Tag() == TYPE_INT )
		return {AdoptRef{}, val_mgr->GetInt(k)};
	else
		return {AdoptRef{}, val_mgr->GetCount(k)};
	}


IntrusivePtr<Val> IncrExpr::Eval(Frame* f) const
	{
	auto v = op->Eval(f);

	if ( ! v )
		return nullptr;

	if ( is_vector(v.get()) )
		{
		IntrusivePtr<VectorVal> v_vec{NewRef{}, v->AsVectorVal()};

		for ( unsigned int i = 0; i < v_vec->Size(); ++i )
			{
			auto elt = v_vec->Lookup(i);

			if ( elt )
				v_vec->Assign(i, DoSingleEval(f, elt.get()));
			else
				v_vec->Assign(i, nullptr);
			}

		op->Assign(f, std::move(v_vec));
		return v;
		}
	else
		{
		auto new_v = DoSingleEval(f, v.get());
		op->Assign(f, new_v);
		return new_v;
		}
	}

bool IncrExpr::IsReduced(Reducer* c) const
	{
	auto ref_op = op->AsRefExpr();
	auto target = ref_op->GetOp1();

	if ( target->Tag() != EXPR_NAME || ! IsIntegral(target->Type()->Tag()) )
		return NonReduced(this);

	return ref_op->IsReduced(c);
	}

Expr* IncrExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( op->Tag() != EXPR_REF )
		Internal("confusion in IncrExpr::Reduce");

	auto ref_op = op->AsRefExpr();
	auto target = ref_op->GetOp1();

	if ( target->Tag() == EXPR_NAME && IsIntegral(target->Type()->Tag()) )
		{
		if ( c->Optimizing() )
			op = c->UpdateExpr(op);
		else
			op = {AdoptRef{}, op->Reduce(c, red_stmt)};

		return this->Ref();
		}

	// First reduce the target's operands to singletons, so that when
	// we re-use it in the assignment below, it has reduced operands.
	auto init_red_stmt = target->ReduceToSingletons(c);

	// Now reduce it all the way to a single value, to use for the
	// increment.
	auto orig_target = target;
	IntrusivePtr<Stmt> target_stmt;
	target = {AdoptRef{}, target->ReduceToSingleton(c, target_stmt)};

	auto incr_const = new ConstExpr({AdoptRef{}, val_mgr->GetCount(1)});
	IntrusivePtr<Expr> incr_ptr = {AdoptRef{}, incr_const};
	incr_const->SetOriginal(this);

	auto incr_expr = Tag() == EXPR_INCR ?
				(Expr*) new AddExpr(target, incr_ptr) :
				(Expr*) new SubExpr(target, incr_ptr);
	incr_expr->SetOriginal(this);
	IntrusivePtr<Stmt> incr_stmt;
	incr_expr = incr_expr->Reduce(c, incr_stmt);

	IntrusivePtr<Stmt> assign_stmt;
	auto rhs = incr_expr->AssignToTemporary(c, assign_stmt);
	IntrusivePtr<Expr> rhs_ptr = {AdoptRef{}, rhs};

	// Build a duplicate version of the original to use as the result.
	if ( orig_target->Tag() == EXPR_NAME )
		orig_target = orig_target->Duplicate();

	else if ( orig_target->Tag() == EXPR_INDEX )
		{
		auto dup1 = orig_target->GetOp1()->Duplicate();
		auto dup2 = orig_target->GetOp2()->Duplicate();
		auto index = dup2->AsListExpr();
		IntrusivePtr<ListExpr> index_ptr = {NewRef{}, index};
		orig_target = make_intrusive<IndexExpr>(dup1, index_ptr);
		}

	else if ( orig_target->Tag() == EXPR_FIELD )
		{
		auto dup1 = orig_target->GetOp1()->Duplicate();
		auto field_name = orig_target->AsFieldExpr()->FieldName();
		orig_target = make_intrusive<FieldExpr>(dup1, field_name);
		}

	else
		reporter->InternalError("confused in IncrExpr::Reduce");

	auto assign = make_intrusive<AssignExpr>(orig_target, rhs_ptr,
						false, nullptr, nullptr, false);

	orig_target->SetOriginal(this);

	// First reduce it regularly, so it can transform into $= or
	// such as needed.  Then reduce that to a singleton to provide
	// the result for this expression.
	IntrusivePtr<Stmt> assign_stmt2;
	auto res = assign->Reduce(c, assign_stmt2);
	res = res->ReduceToSingleton(c, red_stmt);
	red_stmt = MergeStmts(MergeStmts(init_red_stmt, target_stmt),
			MergeStmts(incr_stmt, assign_stmt, assign_stmt2),
				red_stmt);

	return res;
	}

Expr* IncrExpr::ReduceToSingleton(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	auto ref_op = op->AsRefExpr();
	auto target = ref_op->GetOp1();

	if ( target->Tag() == EXPR_NAME && IsIntegral(target->Type()->Tag()) )
		{
		IntrusivePtr<Expr> incr_expr = Duplicate();
		red_stmt = make_intrusive<ExprStmt>(incr_expr);
		red_stmt = {AdoptRef{}, red_stmt->Reduce(c)};

		IntrusivePtr<Stmt> targ_red_stmt;
		auto targ_red = target->Reduce(c, targ_red_stmt);

		red_stmt = MergeStmts(red_stmt, targ_red_stmt);

		return targ_red;
		}

	else
		return UnaryExpr::ReduceToSingleton(c, red_stmt);
	}

const CompiledStmt IncrExpr::Compile(Compiler* c) const
	{
	auto target = op->AsRefExpr()->GetOp1()->AsNameExpr();

	auto s = c->EmptyStmt();

	if ( target->Type()->Tag() == TYPE_INT )
		{
		if ( Tag() == EXPR_INCR )
			s = c->IncrIV(target);
		else
			s = c->DecrIV(target);
		}
	else
		{
		if ( Tag() == EXPR_INCR )
			s = c->IncrUV(target);
		else
			s = c->DecrUV(target);
		}

	auto target_id = target->Id();
	if ( target_id->IsGlobal() )
		// Give the compiler notice that a global may require
		// updating.  We do this explicitly since uni-operand
		// operations assume the operand isn't being modified.
		return c->AssignedToGlobal(target_id);

	return s;
	}

IntrusivePtr<Expr> IncrExpr::Duplicate()
	{
	return SetSucc(new IncrExpr(tag, op->Duplicate()));
	}

bool IncrExpr::IsPure() const
	{
	return false;
	}

bool IncrExpr::HasNoSideEffects() const
	{
	return false;
	}

ComplementExpr::ComplementExpr(IntrusivePtr<Expr> arg_op)
	: UnaryExpr(EXPR_COMPLEMENT, std::move(arg_op))
	{
	if ( IsError() )
		return;

	auto t = op->Type();
	TypeTag bt = t->Tag();

	if ( bt != TYPE_COUNT )
		ExprError("requires \"count\" operand");
	else
		SetType(base_type(TYPE_COUNT));
	}

IntrusivePtr<Val> ComplementExpr::Fold(Val* v) const
	{
	return {AdoptRef{}, val_mgr->GetCount(~ v->InternalUnsigned())};
	}

bool ComplementExpr::WillTransform(Reducer* c) const
	{
	return op->Tag() == EXPR_COMPLEMENT;
	}

Expr* ComplementExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( op->Tag() == EXPR_COMPLEMENT )
		return op->GetOp1().get()->ReduceToSingleton(c, red_stmt);

	return UnaryExpr::Reduce(c, red_stmt);
	}

IntrusivePtr<Expr> ComplementExpr::Duplicate()
	{
	return SetSucc(new ComplementExpr(op->Duplicate()));
	}

NotExpr::NotExpr(IntrusivePtr<Expr> arg_op)
	: UnaryExpr(EXPR_NOT, std::move(arg_op))
	{
	if ( IsError() )
		return;

	TypeTag bt = op->Type()->Tag();

	if ( ! IsIntegral(bt) && bt != TYPE_BOOL )
		ExprError("requires an integral or boolean operand");
	else
		SetType(base_type(TYPE_BOOL));
	}

IntrusivePtr<Val> NotExpr::Fold(Val* v) const
	{
	return {AdoptRef{}, val_mgr->GetBool(! v->InternalInt())};
	}

bool NotExpr::WillTransform(Reducer* c) const
	{
	return op->Tag() == EXPR_NOT && Op()->Type()->Tag() == TYPE_BOOL;
	}

Expr* NotExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( op->Tag() == EXPR_NOT && Op()->Type()->Tag() == TYPE_BOOL )
		return Op()->Reduce(c, red_stmt);

	return UnaryExpr::Reduce(c, red_stmt);
	}

IntrusivePtr<Expr> NotExpr::Duplicate()
	{
	return SetSucc(new NotExpr(op->Duplicate()));
	}

PosExpr::PosExpr(IntrusivePtr<Expr> arg_op)
	: UnaryExpr(EXPR_POSITIVE, std::move(arg_op))
	{
	if ( IsError() )
		return;

	auto t = op->Type().get();

	if ( IsVector(t->Tag()) )
		t = t->AsVectorType()->YieldType();

	TypeTag bt = t->Tag();
	IntrusivePtr<BroType> base_result_type;

	if ( IsIntegral(bt) )
		// Promote count and counter to int.
		base_result_type = base_type(TYPE_INT);
	else if ( bt == TYPE_INTERVAL || bt == TYPE_DOUBLE )
		base_result_type = {NewRef{}, t};
	else
		ExprError("requires an integral or double operand");

	if ( is_vector(op.get()) )
		SetType(make_intrusive<VectorType>(std::move(base_result_type)));
	else
		SetType(std::move(base_result_type));
	}

IntrusivePtr<Val> PosExpr::Fold(Val* v) const
	{
	TypeTag t = v->Type()->Tag();

	if ( t == TYPE_DOUBLE || t == TYPE_INTERVAL || t == TYPE_INT )
		return {NewRef{}, v};
	else
		return {AdoptRef{}, val_mgr->GetInt(v->CoerceToInt())};
	}

IntrusivePtr<Expr> PosExpr::Duplicate()
	{
	return SetSucc(new PosExpr(op->Duplicate()));
	}

bool PosExpr::WillTransform(Reducer* c) const
	{
	return op->Type()->Tag() != TYPE_COUNT;
	}

Expr* PosExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( op->Type()->Tag() == TYPE_COUNT )
		// We need to keep the expression because it leads
		// to a coercion from unsigned to signed.
		return UnaryExpr::Reduce(c, red_stmt);

	else
		return op.get()->ReduceToSingleton(c, red_stmt);
	}

NegExpr::NegExpr(IntrusivePtr<Expr> arg_op)
	: UnaryExpr(EXPR_NEGATE, std::move(arg_op))
	{
	if ( IsError() )
		return;

	BroType* t = op->Type().get();

	if ( IsVector(t->Tag()) )
		t = t->AsVectorType()->YieldType();

	TypeTag bt = t->Tag();
	IntrusivePtr<BroType> base_result_type;

	if ( IsIntegral(bt) )
		// Promote count and counter to int.
		base_result_type = base_type(TYPE_INT);
	else if ( bt == TYPE_INTERVAL || bt == TYPE_DOUBLE )
		base_result_type = {NewRef{}, t};
	else
		ExprError("requires an integral or double operand");

	if ( is_vector(op.get()) )
		SetType(make_intrusive<VectorType>(std::move(base_result_type)));
	else
		SetType(std::move(base_result_type));
	}

IntrusivePtr<Val> NegExpr::Fold(Val* v) const
	{
	if ( v->Type()->Tag() == TYPE_DOUBLE )
		return make_intrusive<Val>(- v->InternalDouble(), v->Type()->Tag());
	else if ( v->Type()->Tag() == TYPE_INTERVAL )
		return make_intrusive<IntervalVal>(- v->InternalDouble(), 1.0);
	else
		return {AdoptRef{}, val_mgr->GetInt(- v->CoerceToInt())};
	}

bool NegExpr::WillTransform(Reducer* c) const
	{
	return op->Tag() == EXPR_NEGATE;
	}

Expr* NegExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( op->Tag() == EXPR_NEGATE )
		return op->GetOp1().get()->ReduceToSingleton(c, red_stmt);

	return UnaryExpr::Reduce(c, red_stmt);
	}

IntrusivePtr<Expr> NegExpr::Duplicate()
	{
	return SetSucc(new NegExpr(op->Duplicate()));
	}

SizeExpr::SizeExpr(IntrusivePtr<Expr> arg_op)
	: UnaryExpr(EXPR_SIZE, std::move(arg_op))
	{
	if ( IsError() )
		return;

	if ( op->Type()->InternalType() == TYPE_INTERNAL_DOUBLE )
		SetType(base_type(TYPE_DOUBLE));
	else
		SetType(base_type(TYPE_COUNT));
	}

IntrusivePtr<Val> SizeExpr::Eval(Frame* f) const
	{
	auto v = op->Eval(f);

	if ( ! v )
		return nullptr;

	return Fold(v.get());
	}

IntrusivePtr<Val> SizeExpr::Fold(Val* v) const
	{
	return v->SizeVal();
	}

IntrusivePtr<Expr> SizeExpr::Duplicate()
	{
	return SetSucc(new SizeExpr(op->Duplicate()));
	}

AddExpr::AddExpr(IntrusivePtr<Expr> arg_op1, IntrusivePtr<Expr> arg_op2)
    : BinaryExpr(EXPR_ADD, std::move(arg_op1), std::move(arg_op2))
	{
	if ( IsError() )
		return;

	TypeTag bt1 = op1->Type()->Tag();

	if ( IsVector(bt1) )
		bt1 = op1->Type()->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = op2->Type()->Tag();

	if ( IsVector(bt2) )
		bt2 = op2->Type()->AsVectorType()->YieldType()->Tag();

	IntrusivePtr<BroType> base_result_type;

	if ( bt2 == TYPE_INTERVAL && ( bt1 == TYPE_TIME || bt1 == TYPE_INTERVAL ) )
		base_result_type = base_type(bt1);
	else if ( bt2 == TYPE_TIME && bt1 == TYPE_INTERVAL )
		base_result_type = base_type(bt2);
	else if ( BothArithmetic(bt1, bt2) )
		PromoteType(max_type(bt1, bt2), is_vector(op1.get()) || is_vector(op2.get()));
	else if ( BothString(bt1, bt2) )
		base_result_type = base_type(bt1);
	else
		ExprError("requires arithmetic operands");

	if ( base_result_type )
		{
		if ( is_vector(op1.get()) || is_vector(op2.get()) )
			SetType(make_intrusive<VectorType>(std::move(base_result_type)));
		else
			SetType(std::move(base_result_type));
		}
	}

void AddExpr::Canonicize()
	{
	if ( expr_greater(op2.get(), op1.get()) ||
	     (op1->Type()->Tag() == TYPE_INTERVAL &&
	      op2->Type()->Tag() == TYPE_TIME) ||
	     (op2->IsConst() && ! is_vector(op2->ExprVal()) && ! op1->IsConst()))
		SwapOps();
	}

bool AddExpr::WillTransform(Reducer* c) const
	{
	return op1->IsZero() || op2->IsZero() ||
		op1->Tag() == EXPR_NEGATE || op2->Tag() == EXPR_NEGATE;
	}

Expr* AddExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( op1->IsZero() )
		return op2.get()->ReduceToSingleton(c, red_stmt);

	if ( op2->IsZero() )
		return op1.get()->ReduceToSingleton(c, red_stmt);

	if ( op1->Tag() == EXPR_NEGATE )
		return BuildSub(op2, op1)->ReduceToSingleton(c, red_stmt);

	if ( op2->Tag() == EXPR_NEGATE )
		return BuildSub(op1, op2)->ReduceToSingleton(c, red_stmt);

	return BinaryExpr::Reduce(c, red_stmt);
	}

IntrusivePtr<Expr> AddExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	return SetSucc(new AddExpr(op1_d, op2_d));
	}

Expr* AddExpr::BuildSub(const IntrusivePtr<Expr>& op1,
			const IntrusivePtr<Expr>& op2)
	{
	auto rhs = op2->GetOp1();
	auto sub = new SubExpr(op1, rhs);
	sub->SetOriginal(this);
	return sub;
	}

AddToExpr::AddToExpr(IntrusivePtr<Expr> arg_op1, IntrusivePtr<Expr> arg_op2)
	: BinaryExpr(EXPR_ADD_TO, is_vector(arg_op1.get()) ?
	             std::move(arg_op1) : arg_op1->MakeLvalue(),
	             std::move(arg_op2))
	{
	if ( IsError() )
		return;

	TypeTag bt1 = op1->Type()->Tag();
	TypeTag bt2 = op2->Type()->Tag();

	if ( BothArithmetic(bt1, bt2) )
		PromoteType(max_type(bt1, bt2), is_vector(op1.get()) || is_vector(op2.get()));
	else if ( BothString(bt1, bt2) || BothInterval(bt1, bt2) )
		SetType(base_type(bt1));

	else if ( IsVector(bt1) )
		{
		bt1 = op1->Type()->AsVectorType()->YieldType()->Tag();

		if ( IsArithmetic(bt1) )
			{
			if ( IsArithmetic(bt2) )
				{
				if ( bt2 != bt1 )
					op2 = make_intrusive<ArithCoerceExpr>(std::move(op2), bt1);

				SetType(op1->Type());
				}

			else
				ExprError("appending non-arithmetic to arithmetic vector");
			}

		else if ( bt1 != bt2 && bt1 != TYPE_ANY )
			ExprError(fmt("incompatible vector append: %s and %s",
					  type_name(bt1), type_name(bt2)));

		else
			SetType(op1->Type());
		}

	else
		ExprError("requires two arithmetic or two string operands");
	}

IntrusivePtr<Val> AddToExpr::Eval(Frame* f) const
	{
	auto v1 = op1->Eval(f);

	if ( ! v1 )
		return nullptr;

	auto v2 = op2->Eval(f);

	if ( ! v2 )
		return nullptr;

	if ( is_vector(v1.get()) )
		{
		VectorVal* vv = v1->AsVectorVal();

		if ( ! vv->Assign(vv->Size(), v2) )
			RuntimeError("type-checking failed in vector append");

		return v1;
		}

	if ( auto result = Fold(v1.get(), v2.get()) )
		{
		op1->Assign(f, result);
		return result;
		}
	else
		return nullptr;
	}

Expr* AddToExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( IsVector(op1->Type()->Tag()) )
		{
		IntrusivePtr<Stmt> red_stmt1;
		IntrusivePtr<Stmt> red_stmt2;

		if ( op1->Tag() == EXPR_FIELD )
			red_stmt1 = op1->ReduceToSingletons(c);
		else
			op1 = {AdoptRef{}, op1->Reduce(c, red_stmt1)};

		op2 = {AdoptRef{}, op2->Reduce(c, red_stmt2)};

		auto append = new AppendToExpr(op1->Duplicate(), op2);
		append->SetOriginal(this);

		IntrusivePtr<Expr> append_ptr = {AdoptRef{}, append};
		auto append_stmt = make_intrusive<ExprStmt>(append_ptr);

		red_stmt = MergeStmts(red_stmt1, red_stmt2, append_stmt);

		return op1->Ref();
		}

	else
		{
		// We could do an ASSERT that op1 is an EXPR_REF, but
		// the following is basically equivalent.
		auto rhs = op1->AsRefExpr()->GetOp1();
		auto do_incr = make_intrusive<AddExpr>(rhs->Duplicate(), op2);
		auto assign = new AssignExpr(op1, do_incr, false, nullptr,
						nullptr, false);

		return assign->ReduceToSingleton(c, red_stmt);
		}
	}

IntrusivePtr<Expr> AddToExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	return SetSucc(new AddToExpr(op1_d, op2_d));
	}

AppendToExpr::AppendToExpr(IntrusivePtr<Expr> arg_op1,
				IntrusivePtr<Expr> arg_op2)
	: BinaryExpr(EXPR_APPEND_TO, std::move(arg_op1), std::move(arg_op2))
	{
	// This is an internal type, so we don't bother with type-checking
	// or coercions, those have already been done before we're created.
	SetType(op1->Type());
	}

IntrusivePtr<Val> AppendToExpr::Eval(Frame* f) const
	{
	auto v1 = op1->Eval(f);

	if ( ! v1 )
		return nullptr;

	auto v2 = op2->Eval(f);

	if ( ! v2 )
		return nullptr;

	VectorVal* vv = v1->AsVectorVal();

	if ( ! vv->Assign(vv->Size(), v2) )
		RuntimeError("type-checking failed in vector append");

	return v1;
	}

bool AppendToExpr::IsReduced(Reducer* c) const
	{
	// These are created reduced.
	return true;
	}

Expr* AppendToExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( c->Optimizing() )
		{
		op1 = c->UpdateExpr(op1);
		op2 = c->UpdateExpr(op2);
		}

	return this->Ref();
	}

const CompiledStmt AppendToExpr::Compile(Compiler* c) const
	{
	auto n2 = op2->Tag() == EXPR_NAME ? op2->AsNameExpr() : nullptr;
	auto cc = op2->Tag() != EXPR_NAME ? op2->AsConstExpr() : nullptr;

	if ( op1->Tag() == EXPR_FIELD )
		{
		auto f = op1->AsFieldExpr()->Field();
		auto n1 = op1->GetOp1()->AsNameExpr();
		return c->AppendToField(n1, n2, cc, f);
		}

	auto n1 = op1->AsNameExpr();

	return n2 ? c->AppendToVV(n1, n2) : c->AppendToVC(n1, cc);
	}

IntrusivePtr<Expr> AppendToExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	return SetSucc(new AppendToExpr(op1_d, op2_d));
	}


SubExpr::SubExpr(IntrusivePtr<Expr> arg_op1, IntrusivePtr<Expr> arg_op2)
	: BinaryExpr(EXPR_SUB, std::move(arg_op1), std::move(arg_op2))
	{
	if ( IsError() )
		return;

	auto t1 = op1->Type();
	auto t2 = op2->Type();

	TypeTag bt1 = t1->Tag();
	if ( IsVector(bt1) )
		bt1 = t1->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = t2->Tag();
	if ( IsVector(bt2) )
		bt2 = t2->AsVectorType()->YieldType()->Tag();

	IntrusivePtr<BroType> base_result_type;

	if ( bt2 == TYPE_INTERVAL && (bt1 == TYPE_TIME || bt1 == TYPE_INTERVAL) )
		base_result_type = base_type(bt1);

	else if ( bt1 == TYPE_TIME && bt2 == TYPE_TIME )
		SetType(base_type(TYPE_INTERVAL));

	else if ( t1->IsSet() && t2->IsSet() )
		{
		if ( same_type(t1, t2) )
			SetType(op1->Type());
		else
			ExprError("incompatible \"set\" operands");
		}

	else if ( BothArithmetic(bt1, bt2) )
		PromoteType(max_type(bt1, bt2), is_vector(op1.get()) || is_vector(op2.get()));

	else
		ExprError("requires arithmetic operands");

	if ( base_result_type )
		{
		if ( is_vector(op1.get()) || is_vector(op2.get()) )
			SetType(make_intrusive<VectorType>(std::move(base_result_type)));
		else
			SetType(std::move(base_result_type));
		}
	}

bool SubExpr::WillTransform(Reducer* c) const
	{
	return op2->IsZero() || op2->Tag() == EXPR_NEGATE || 
		(type->Tag() != TYPE_VECTOR && type->Tag() != TYPE_TABLE &&
	         op1->Tag() == EXPR_NAME && op2->Tag() == EXPR_NAME &&
		 op1->AsNameExpr()->Id() == op2->AsNameExpr()->Id());
	}

Expr* SubExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( op2->IsZero() )
		return op1.get()->ReduceToSingleton(c, red_stmt);

	if ( op2->Tag() == EXPR_NEGATE )
		{
		auto rhs = op2->GetOp1();
		auto add = new AddExpr(op1, rhs);
		add->SetOriginal(this);
		return add->Reduce(c, red_stmt);
		}

	if ( c->Optimizing() )
		{ // Allow for alias expansion.
		op1 = c->UpdateExpr(op1);
		op2 = c->UpdateExpr(op2);
		}

	if ( type->Tag() != TYPE_VECTOR && type->Tag() != TYPE_TABLE &&
	     op1->Tag() == EXPR_NAME && op2->Tag() == EXPR_NAME )
		{
		auto n1 = op1->AsNameExpr();
		auto n2 = op2->AsNameExpr();
		if ( n1->Id() == n2->Id() )
			{
			auto zero = MakeZeroExpr(type->Tag());
			return TransformMe(zero, c, red_stmt);
			}
		}

	return BinaryExpr::Reduce(c, red_stmt);
	}

IntrusivePtr<Expr> SubExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	return SetSucc(new SubExpr(op1_d, op2_d));
	}

RemoveFromExpr::RemoveFromExpr(IntrusivePtr<Expr> arg_op1,
				IntrusivePtr<Expr> arg_op2)
	: BinaryExpr(EXPR_REMOVE_FROM, arg_op1->MakeLvalue(), std::move(arg_op2))
	{
	if ( IsError() )
		return;

	TypeTag bt1 = op1->Type()->Tag();
	TypeTag bt2 = op2->Type()->Tag();

	if ( BothArithmetic(bt1, bt2) )
		PromoteType(max_type(bt1, bt2), is_vector(op1.get()) || is_vector(op2.get()));
	else if ( BothInterval(bt1, bt2) )
		SetType(base_type(bt1));
	else
		ExprError("requires two arithmetic operands");
	}

IntrusivePtr<Val> RemoveFromExpr::Eval(Frame* f) const
	{
	auto v1 = op1->Eval(f);

	if ( ! v1 )
		return nullptr;

	auto v2 = op2->Eval(f);

	if ( ! v2 )
		return nullptr;

	if ( auto result = Fold(v1.get(), v2.get()) )
		{
		op1->Assign(f, result);
		return result;
		}
	else
		return nullptr;
	}

Expr* RemoveFromExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	auto rhs = op1->AsRefExpr()->GetOp1();
	auto do_decr = make_intrusive<SubExpr>(rhs->Duplicate(), op2);
	auto assign = new AssignExpr(op1, do_decr, false, nullptr, nullptr,
					false);

	return assign->Reduce(c, red_stmt);
	}

IntrusivePtr<Expr> RemoveFromExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	return SetSucc(new RemoveFromExpr(op1_d, op2_d));
	}

TimesExpr::TimesExpr(IntrusivePtr<Expr> arg_op1, IntrusivePtr<Expr> arg_op2)
	: BinaryExpr(EXPR_TIMES, std::move(arg_op1), std::move(arg_op2))
	{
	if ( IsError() )
		return;

	Canonicize();

	TypeTag bt1 = op1->Type()->Tag();

	if ( IsVector(bt1) )
		bt1 = op1->Type()->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = op2->Type()->Tag();

	if ( IsVector(bt2) )
		bt2 = op2->Type()->AsVectorType()->YieldType()->Tag();

	if ( bt1 == TYPE_INTERVAL || bt2 == TYPE_INTERVAL )
		{
		if ( IsArithmetic(bt1) || IsArithmetic(bt2) )
			PromoteForInterval(IsArithmetic(bt1) ? op1 : op2);
		else
			ExprError("multiplication with interval requires arithmetic operand");
		}
	else if ( BothArithmetic(bt1, bt2) )
		PromoteType(max_type(bt1, bt2), is_vector(op1.get()) || is_vector(op2.get()));
	else
		ExprError("requires arithmetic operands");
	}

void TimesExpr::Canonicize()
	{
	if ( expr_greater(op2.get(), op1.get()) || op2->Type()->Tag() == TYPE_INTERVAL ||
	     (op2->IsConst() && ! is_vector(op2->ExprVal()) && ! op1->IsConst()) )
		SwapOps();
	}

bool TimesExpr::WillTransform(Reducer* c) const
	{
	return op1->IsZero() || op2->IsZero() || op1->IsOne() || op2->IsOne();
	}

Expr* TimesExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( op1->IsOne() )
		return op2.get()->ReduceToSingleton(c, red_stmt);

	if ( op2->IsOne() )
		return op1.get()->ReduceToSingleton(c, red_stmt);

	// Optimize integral multiplication by zero ... but not
	// double, due to cases like Inf*0 or NaN*0.
	if ( (op1->IsZero() || op2->IsZero()) && Type()->Tag() != TYPE_DOUBLE )
		{
		auto zero_val = op1->IsZero() ?
				op1->Eval(nullptr) : op2->Eval(nullptr);
		return new ConstExpr(zero_val);
		}

	return BinaryExpr::Reduce(c, red_stmt);
	}

IntrusivePtr<Expr> TimesExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	return SetSucc(new TimesExpr(op1_d, op2_d));
	}

DivideExpr::DivideExpr(IntrusivePtr<Expr> arg_op1,
                       IntrusivePtr<Expr> arg_op2)
	: BinaryExpr(EXPR_DIVIDE, std::move(arg_op1), std::move(arg_op2))
	{
	if ( IsError() )
		return;

	TypeTag bt1 = op1->Type()->Tag();

	if ( IsVector(bt1) )
		bt1 = op1->Type()->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = op2->Type()->Tag();

	if ( IsVector(bt2) )
		bt2 = op2->Type()->AsVectorType()->YieldType()->Tag();

	if ( bt1 == TYPE_INTERVAL || bt2 == TYPE_INTERVAL )
		{
		if ( IsArithmetic(bt1) || IsArithmetic(bt2) )
			PromoteForInterval(IsArithmetic(bt1) ? op1 : op2);
		else if ( bt1 == TYPE_INTERVAL && bt2 == TYPE_INTERVAL )
			{
			if ( is_vector(op1.get()) || is_vector(op2.get()) )
				SetType(make_intrusive<VectorType>(base_type(TYPE_DOUBLE)));
			else
				SetType(base_type(TYPE_DOUBLE));
			}
		else
			ExprError("division of interval requires arithmetic operand");
		}

	else if ( BothArithmetic(bt1, bt2) )
		PromoteType(max_type(bt1, bt2), is_vector(op1.get()) || is_vector(op2.get()));

	else if ( bt1 == TYPE_ADDR && ! is_vector(op2.get()) &&
		  (bt2 == TYPE_COUNT || bt2 == TYPE_INT) )
		{
		if ( bt2 != TYPE_INT )
			op2 = make_intrusive<ArithCoerceExpr>(std::move(op2), TYPE_INT);
		SetType(base_type(TYPE_SUBNET));
		}

	else
		ExprError("requires arithmetic operands");
	}

bool DivideExpr::WillTransform(Reducer* c) const
	{
	return Type()->Tag() != TYPE_SUBNET && op2->IsOne();
	}

Expr* DivideExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( Type()->Tag() != TYPE_SUBNET )
		{
		if ( op2->IsOne() )
			return op1.get()->ReduceToSingleton(c, red_stmt);
		}

	return BinaryExpr::Reduce(c, red_stmt);
	}

IntrusivePtr<Expr> DivideExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	return SetSucc(new DivideExpr(op1_d, op2_d));
	}

IntrusivePtr<Val> DivideExpr::AddrFold(Val* v1, Val* v2) const
	{
	uint32_t mask;

	if ( v2->Type()->Tag() == TYPE_COUNT )
		mask = static_cast<uint32_t>(v2->InternalUnsigned());
	else
		mask = static_cast<uint32_t>(v2->InternalInt());

	auto& a = v1->AsAddr();

	if ( a.GetFamily() == IPv4 )
		{
		if ( mask > 32 )
			RuntimeError(fmt("bad IPv4 subnet prefix length: %" PRIu32, mask));
		}
	else
		{
		if ( mask > 128 )
			RuntimeError(fmt("bad IPv6 subnet prefix length: %" PRIu32, mask));
		}

	return make_intrusive<SubNetVal>(a, mask);
	}

ModExpr::ModExpr(IntrusivePtr<Expr> arg_op1, IntrusivePtr<Expr> arg_op2)
	: BinaryExpr(EXPR_MOD, std::move(arg_op1), std::move(arg_op2))
	{
	if ( IsError() )
		return;

	TypeTag bt1 = op1->Type()->Tag();

	if ( IsVector(bt1) )
		bt1 = op1->Type()->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = op2->Type()->Tag();

	if ( IsVector(bt2) )
		bt2 = op2->Type()->AsVectorType()->YieldType()->Tag();

	if ( BothIntegral(bt1, bt2) )
		PromoteType(max_type(bt1, bt2), is_vector(op1.get()) || is_vector(op2.get()));
	else
		ExprError("requires integral operands");
	}

IntrusivePtr<Expr> ModExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	return SetSucc(new ModExpr(op1_d, op2_d));
	}

BoolExpr::BoolExpr(BroExprTag arg_tag, IntrusivePtr<Expr> arg_op1,
					IntrusivePtr<Expr> arg_op2)
	: BinaryExpr(arg_tag, std::move(arg_op1), std::move(arg_op2))
	{
	if ( IsError() )
		return;

	TypeTag bt1 = op1->Type()->Tag();

	if ( IsVector(bt1) )
		bt1 = op1->Type()->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = op2->Type()->Tag();

	if ( IsVector(bt2) )
		bt2 = op2->Type()->AsVectorType()->YieldType()->Tag();

	if ( BothBool(bt1, bt2) )
		{
		if ( is_vector(op1.get()) || is_vector(op2.get()) )
			{
			if ( ! (is_vector(op1.get()) && is_vector(op2.get())) )
				reporter->Warning("mixing vector and scalar operands is deprecated");
			SetType(make_intrusive<VectorType>(base_type(TYPE_BOOL)));
			}
		else
			SetType(base_type(TYPE_BOOL));
		}
	else
		ExprError("requires boolean operands");
	}

IntrusivePtr<Val> BoolExpr::DoSingleEval(Frame* f, IntrusivePtr<Val> v1, Expr* op2) const
	{
	if ( ! v1 )
		return nullptr;

	if ( tag == EXPR_AND_AND )
		{
		if ( v1->IsZero() )
			return v1;
		else
			return op2->Eval(f);
		}

	else
		{
		if ( v1->IsZero() )
			return op2->Eval(f);
		else
			return v1;
		}
	}

IntrusivePtr<Val> BoolExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return nullptr;

	auto v1 = op1->Eval(f);

	if ( ! v1 )
		return nullptr;

	bool is_vec1 = is_vector(op1.get());
	bool is_vec2 = is_vector(op2.get());

	// Handle scalar op scalar
	if ( ! is_vec1 && ! is_vec2 )
		return DoSingleEval(f, std::move(v1), op2.get());

	// Handle scalar op vector  or  vector op scalar
	// We can't short-circuit everything since we need to eval
	// a vector in order to find out its length.
	if ( ! (is_vec1 && is_vec2) )
		{ // Only one is a vector.
		IntrusivePtr<Val> scalar_v;
		IntrusivePtr<VectorVal> vector_v;

		if ( is_vec1 )
			{
			scalar_v = op2->Eval(f);
			vector_v = {AdoptRef{}, v1.release()->AsVectorVal()};
			}
		else
			{
			scalar_v = std::move(v1);
			vector_v = {AdoptRef{}, op2->Eval(f).release()->AsVectorVal()};
			}

		if ( ! scalar_v || ! vector_v )
			return nullptr;

		IntrusivePtr<VectorVal> result;

		// It's either an EXPR_AND_AND or an EXPR_OR_OR.
		bool is_and = (tag == EXPR_AND_AND);

		if ( scalar_v->IsZero() == is_and )
			{
			result = make_intrusive<VectorVal>(Type()->AsVectorType());
			result->Resize(vector_v->Size());
			result->AssignRepeat(0, result->Size(), scalar_v.get());
			}
		else
			result = std::move(vector_v);

		return result;
		}

	// Only case remaining: both are vectors.
	auto v2 = op2->Eval(f);

	if ( ! v2 )
		return nullptr;

	VectorVal* vec_v1 = v1->AsVectorVal();
	VectorVal* vec_v2 = v2->AsVectorVal();

	if ( vec_v1->Size() != vec_v2->Size() )
		{
		RuntimeError("vector operands have different sizes");
		return nullptr;
		}

	auto result = make_intrusive<VectorVal>(Type()->AsVectorType());
	result->Resize(vec_v1->Size());

	for ( unsigned int i = 0; i < vec_v1->Size(); ++i )
		{
		auto op1 = vec_v1->Lookup(i);
		auto op2 = vec_v2->Lookup(i);
		if ( op1 && op2 )
			{
			bool local_result = (tag == EXPR_AND_AND) ?
				(! op1->IsZero() && ! op2->IsZero()) :
				(! op1->IsZero() || ! op2->IsZero());

			result->Assign(i, val_mgr->GetBool(local_result));
			}
		else
			result->Assign(i, 0);
		}

	return result;
	}

// Returns true if the given Expr is either of the form "/pat/ in var" or a
// (possibly extended) "||" disjunction of such nodes, for which "var" is
// always the same.  If true, returns the ID* corresponding to "var", and
// collects the associated pattern constants in "patterns".
//
// Note that for an initial (non-recursive) call, "id" should be set to
// nullptr, and the caller should have ensured that the starting point is
// a disjunction (since a bare "/pat/ in var" by itself isn't a "cascade"
// and doesn't present a potential optimization opportunity.
static bool is_pattern_cascade(const Expr* e, ID*& id,
				std::vector<ConstExpr*>& patterns)
	{
	auto lhs = e->GetOp1();
	auto rhs = e->GetOp2();

	if ( e->Tag() == EXPR_IN )
		{
		if ( lhs->Tag() != EXPR_CONST ||
		     lhs->Type()->Tag() != TYPE_PATTERN ||
		     rhs->Tag() != EXPR_NAME )
			return false;

		auto rhs_id = rhs->AsNameExpr()->Id();

		if ( id && rhs_id != id )
			return false;

		id = rhs_id;
		patterns.push_back(lhs->AsConstExpr());

		return true;
		}

	if ( e->Tag() != EXPR_OR_OR )
		return false;

	return is_pattern_cascade(lhs.get(), id, patterns) &&
		is_pattern_cascade(rhs.get(), id, patterns);
	}

// Given a set of pattern constants, returns a disjunction that
// includes all of them.
static IntrusivePtr<Expr> BuildDisjunction(std::vector<ConstExpr*>& patterns)
	{
	ASSERT(patterns.size() > 1);

	IntrusivePtr<Expr> e = {NewRef{}, patterns[0]};

	for ( unsigned int i = 1; i < patterns.size(); ++i )
		{
		IntrusivePtr<Expr> e_i = {NewRef{}, patterns[i]};
		e = make_intrusive<BitExpr>(EXPR_OR, e, e_i);
		}

	return e;
	}

bool BoolExpr::WillTransformInConditional(Reducer* c) const
	{
	ID* common_id = nullptr;
	std::vector<ConstExpr*> patterns;
	return tag == EXPR_OR_OR &&
		is_pattern_cascade(this, common_id, patterns);
	}

Expr* BoolExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	// First, look for a common idiom of "/foo/ in x || /bar/ in x"
	// and translate it to "(/foo/ | /bar) in x", which is more
	// efficient to match.
	ID* common_id = nullptr;
	std::vector<ConstExpr*> patterns;
	if ( tag == EXPR_OR_OR &&
	     is_pattern_cascade(this, common_id, patterns) )
		{
		auto new_pat = BuildDisjunction(patterns);
		IntrusivePtr<ID> common_id_ptr = {NewRef{}, common_id};
		auto new_id = make_intrusive<NameExpr>(common_id_ptr);
		auto new_node = make_intrusive<InExpr>(new_pat, new_id);
		return new_node->Reduce(c, red_stmt);
		}

	// It's either an EXPR_AND_AND or an EXPR_OR_OR.
	bool is_and = (tag == EXPR_AND_AND);

	if ( IsTrue(op1) )
		{
		if ( is_and )
			return op2->ReduceToSingleton(c, red_stmt);
		else
			return op1->ReduceToSingleton(c, red_stmt);
		}

	if ( IsFalse(op1) )
		{
		if ( is_and )
			return op1->ReduceToSingleton(c, red_stmt);
		else
			return op2->ReduceToSingleton(c, red_stmt);
		}

	if ( op1->HasNoSideEffects() )
		{
		if ( IsTrue(op2) )
			{
			if ( is_and )
				return op1->ReduceToSingleton(c, red_stmt);
			else
				return op2->ReduceToSingleton(c, red_stmt);
			}

		if ( IsFalse(op2) )
			{
			if ( is_and )
				return op2->ReduceToSingleton(c, red_stmt);
			else
				return op1->ReduceToSingleton(c, red_stmt);
			}
		}

	auto else_val = is_and ? val_mgr->GetFalse() : val_mgr->GetTrue();
	IntrusivePtr<Val> else_val_int = {AdoptRef{}, else_val};
	IntrusivePtr<Expr> else_e = {AdoptRef{}, new ConstExpr(else_val_int)};

	Expr* cond;
	if ( is_and )
		cond = new CondExpr(op1, op2, else_e);
	else
		cond = new CondExpr(op1, else_e, op2);

	auto cond_red = cond->ReduceToSingleton(c, red_stmt);

	return TransformMe(cond_red, c, red_stmt);
	}

IntrusivePtr<Expr> BoolExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	return SetSucc(new BoolExpr(tag, op1_d, op2_d));
	}

bool BoolExpr::IsTrue(const IntrusivePtr<Expr>& e) const
	{
	if ( ! e->IsConst() )
		return false;

	auto c_e = e->AsConstExpr();
	return c_e->Value()->IsOne();
	}

bool BoolExpr::IsFalse(const IntrusivePtr<Expr>& e) const
	{
	if ( ! e->IsConst() )
		return false;

	auto c_e = e->AsConstExpr();
	return c_e->Value()->IsZero();
	}


BitExpr::BitExpr(BroExprTag arg_tag,
                 IntrusivePtr<Expr> arg_op1, IntrusivePtr<Expr> arg_op2)
	: BinaryExpr(arg_tag, std::move(arg_op1), std::move(arg_op2))
	{
	if ( IsError() )
		return;

	auto t1 = op1->Type();
	auto t2 = op2->Type();

	TypeTag bt1 = t1->Tag();

	if ( IsVector(bt1) )
		bt1 = t1->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = t2->Tag();

	if ( IsVector(bt2) )
		bt2 = t2->AsVectorType()->YieldType()->Tag();

	if ( (bt1 == TYPE_COUNT || bt1 == TYPE_COUNTER) &&
	     (bt2 == TYPE_COUNT || bt2 == TYPE_COUNTER) )
		{
		if ( bt1 == TYPE_COUNTER && bt2 == TYPE_COUNTER )
			ExprError("cannot apply a bitwise operator to two \"counter\" operands");
		else if ( is_vector(op1.get()) || is_vector(op2.get()) )
			SetType(make_intrusive<VectorType>(base_type(TYPE_COUNT)));
		else
			SetType(base_type(TYPE_COUNT));
		}

	else if ( bt1 == TYPE_PATTERN )
		{
		if ( bt2 != TYPE_PATTERN )
			ExprError("cannot mix pattern and non-pattern operands");
		else if ( tag == EXPR_XOR )
			ExprError("'^' operator does not apply to patterns");
		else
			SetType(base_type(TYPE_PATTERN));
		}

	else if ( t1->IsSet() && t2->IsSet() )
		{
		if ( same_type(t1, t2) )
			SetType(op1->Type());
		else
			ExprError("incompatible \"set\" operands");
		}

	else
		ExprError("requires \"count\" or compatible \"set\" operands");
	}

bool BitExpr::WillTransform(Reducer* c) const
	{
	return Type()->Tag() == TYPE_COUNT &&
		(op1->IsZero() || op2->IsZero() ||
		 (same_singletons(op1, op2) && op1->Tag() == EXPR_NAME));
	}

Expr* BitExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( Type()->Tag() != TYPE_COUNT )
		return BinaryExpr::Reduce(c, red_stmt);

	auto zero1 = op1->IsZero();
	auto zero2 = op2->IsZero();

	if ( zero1 && zero2 )
		// No matter the operation, the answer is zero.
		return op1->ReduceToSingleton(c, red_stmt);

	if ( zero1 || zero2 )
		{
		IntrusivePtr<Expr>& zero_op = zero1 ? op1 : op2;
		IntrusivePtr<Expr>& non_zero_op = zero1 ? op2 : op1;

		if ( Tag() == EXPR_AND )
			return zero_op->ReduceToSingleton(c, red_stmt);
		else
			// OR or XOR
			return non_zero_op->ReduceToSingleton(c, red_stmt);
		}

	if ( same_singletons(op1, op2) && op1->Tag() == EXPR_NAME )
		{
		auto n = op1->AsNameExpr();

		if ( Tag() == EXPR_XOR )
			{
			auto zero = new ConstExpr({AdoptRef{},
						val_mgr->GetCount(0)});
			zero->SetOriginal(this);
			return zero->Reduce(c, red_stmt);
			}

		else
			return op1->ReduceToSingleton(c, red_stmt);
		}

	return BinaryExpr::Reduce(c, red_stmt);
	}

IntrusivePtr<Expr> BitExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	return SetSucc(new BitExpr(tag, op1_d, op2_d));
	}

EqExpr::EqExpr(BroExprTag arg_tag,
               IntrusivePtr<Expr> arg_op1, IntrusivePtr<Expr> arg_op2)
	: BinaryExpr(arg_tag, std::move(arg_op1), std::move(arg_op2))
	{
	if ( IsError() )
		return;

	Canonicize();

	auto t1 = op1->Type();
	auto t2 = op2->Type();

	TypeTag bt1 = t1->Tag();
	if ( IsVector(bt1) )
		bt1 = t1->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = t2->Tag();
	if ( IsVector(bt2) )
		bt2 = t2->AsVectorType()->YieldType()->Tag();

	if ( is_vector(op1.get()) || is_vector(op2.get()) )
		SetType(make_intrusive<VectorType>(base_type(TYPE_BOOL)));
	else
		SetType(base_type(TYPE_BOOL));

	if ( BothArithmetic(bt1, bt2) )
		PromoteOps(max_type(bt1, bt2));

	else if ( EitherArithmetic(bt1, bt2) &&
		// Allow comparisons with zero.
		  ((bt1 == TYPE_TIME && op2->IsZero()) ||
		   (bt2 == TYPE_TIME && op1->IsZero())) )
		PromoteOps(TYPE_TIME);

	else if ( bt1 == bt2 )
		{
		switch ( bt1 ) {
		case TYPE_BOOL:
		case TYPE_TIME:
		case TYPE_INTERVAL:
		case TYPE_STRING:
		case TYPE_PORT:
		case TYPE_ADDR:
		case TYPE_SUBNET:
		case TYPE_ERROR:
			break;

		case TYPE_ENUM:
			if ( ! same_type(t1, t2) )
				ExprError("illegal enum comparison");
			break;

		case TYPE_TABLE:
			if ( t1->IsSet() && t2->IsSet() )
				{
				if ( ! same_type(t1, t2) )
					ExprError("incompatible sets in comparison");
				break;
				}

			// FALL THROUGH

		default:
			ExprError("illegal comparison");
		}
		}

	else if ( bt1 == TYPE_PATTERN && bt2 == TYPE_STRING )
		;

	else
		ExprError("type clash in comparison");
	}

void EqExpr::Canonicize()
	{
	if ( op2->Type()->Tag() == TYPE_PATTERN )
		SwapOps();

	else if ( op1->Type()->Tag() == TYPE_PATTERN )
		;

	else if ( expr_greater(op2.get(), op1.get()) )
		SwapOps();
	}

IntrusivePtr<Val> EqExpr::Fold(Val* v1, Val* v2) const
	{
	if ( op1->Type()->Tag() == TYPE_PATTERN )
		{
		RE_Matcher* re = v1->AsPattern();
		const BroString* s = v2->AsString();
		if ( tag == EXPR_EQ )
			return {AdoptRef{}, val_mgr->GetBool(re->MatchExactly(s))};
		else
			return {AdoptRef{}, val_mgr->GetBool(! re->MatchExactly(s))};
		}

	else
		return BinaryExpr::Fold(v1, v2);
	}

bool EqExpr::WillTransform(Reducer* c) const
	{
	return Type()->Tag() == TYPE_BOOL && same_singletons(op1, op2);
	}

Expr* EqExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( Type()->Tag() == TYPE_BOOL && same_singletons(op1, op2) )
		{
		bool t = Tag() == EXPR_EQ;
		auto res = new ConstExpr({AdoptRef{}, val_mgr->GetBool(t)});
		res->SetOriginal(this);
		return res->Reduce(c, red_stmt);
		}

	return BinaryExpr::Reduce(c, red_stmt);
	}

IntrusivePtr<Expr> EqExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	return SetSucc(new EqExpr(tag, op1_d, op2_d));
	}

RelExpr::RelExpr(BroExprTag arg_tag,
                 IntrusivePtr<Expr> arg_op1, IntrusivePtr<Expr> arg_op2)
	: BinaryExpr(arg_tag, std::move(arg_op1), std::move(arg_op2))
	{
	if ( IsError() )
		return;

	Canonicize();

	auto t1 = op1->Type();
	auto t2 = op2->Type();

	TypeTag bt1 = t1->Tag();
	if ( IsVector(bt1) )
		bt1 = t1->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = t2->Tag();
	if ( IsVector(bt2) )
		bt2 = t2->AsVectorType()->YieldType()->Tag();

	if ( is_vector(op1.get()) || is_vector(op2.get()) )
		SetType(make_intrusive<VectorType>(base_type(TYPE_BOOL)));
	else
		SetType(base_type(TYPE_BOOL));

	if ( BothArithmetic(bt1, bt2) )
		PromoteOps(max_type(bt1, bt2));

	else if ( t1->IsSet() && t2->IsSet() )
		{
		if ( ! same_type(t1, t2) )
			ExprError("incompatible sets in comparison");
		}

	else if ( bt1 != bt2 )
		ExprError("operands must be of the same type");

	else if ( bt1 != TYPE_TIME && bt1 != TYPE_INTERVAL &&
		  bt1 != TYPE_PORT && bt1 != TYPE_ADDR &&
		  bt1 != TYPE_STRING )
		ExprError("illegal comparison");
	}

void RelExpr::Canonicize()
	{
	if ( tag == EXPR_GT )
		{
		SwapOps();
		tag = EXPR_LT;
		}

	else if ( tag == EXPR_GE )
		{
		SwapOps();
		tag = EXPR_LE;
		}
	}

bool RelExpr::WillTransform(Reducer* c) const
	{
	return Type()->Tag() == TYPE_BOOL && same_singletons(op1, op2);
	}

Expr* RelExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( Type()->Tag() == TYPE_BOOL )
		{
		if ( same_singletons(op1, op2) )
			{
			bool t = Tag() == EXPR_GE || Tag() == EXPR_LE;
			auto res = new ConstExpr({AdoptRef{},
						val_mgr->GetBool(t)});
			res->SetOriginal(this);
			return res->Reduce(c, red_stmt);
			}

		if ( op1->IsZero() && op2->Type()->Tag() == TYPE_COUNT &&
		     (Tag() == EXPR_LE || Tag() == EXPR_GT) )
			Warn("degenerate comparison");

		if ( op2->IsZero() && op1->Type()->Tag() == TYPE_COUNT &&
		     (Tag() == EXPR_LT || Tag() == EXPR_GE) )
			Warn("degenerate comparison");
		}

	return BinaryExpr::Reduce(c, red_stmt);
	}

IntrusivePtr<Expr> RelExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	return SetSucc(new RelExpr(tag, op1_d, op2_d));
	}

CondExpr::CondExpr(IntrusivePtr<Expr> arg_op1, IntrusivePtr<Expr> arg_op2,
                   IntrusivePtr<Expr> arg_op3)
	: Expr(EXPR_COND),
	  op1(std::move(arg_op1)), op2(std::move(arg_op2)), op3(std::move(arg_op3))
	{
	TypeTag bt1 = op1->Type()->Tag();

	if ( IsVector(bt1) )
		bt1 = op1->Type()->AsVectorType()->YieldType()->Tag();

	if ( op1->IsError() || op2->IsError() || op3->IsError() )
		SetError();

	else if ( bt1 != TYPE_BOOL )
		ExprError("requires boolean conditional");

	else
		{
		TypeTag bt2 = op2->Type()->Tag();

		if ( is_vector(op2.get()) )
			bt2 = op2->Type()->AsVectorType()->YieldType()->Tag();

		TypeTag bt3 = op3->Type()->Tag();

		if ( IsVector(bt3) )
			bt3 = op3->Type()->AsVectorType()->YieldType()->Tag();

		if ( is_vector(op1.get()) && ! (is_vector(op2.get()) && is_vector(op3.get())) )
			{
			ExprError("vector conditional requires vector alternatives");
			return;
			}

		if ( BothArithmetic(bt2, bt3) )
			{
			TypeTag t = max_type(bt2, bt3);
			if ( bt2 != t )
				op2 = make_intrusive<ArithCoerceExpr>(std::move(op2), t);
			if ( bt3 != t )
				op3 = make_intrusive<ArithCoerceExpr>(std::move(op3), t);

			if ( is_vector(op2.get()) )
				SetType(make_intrusive<VectorType>(base_type(t)));
			else
				SetType(base_type(t));
			}

		else if ( bt2 != bt3 )
			ExprError("operands must be of the same type");

		else
			{
			if ( IsRecord(bt2) && IsRecord(bt3) &&
			     ! same_type(op2->Type(), op3->Type()) )
				ExprError("operands must be of the same type");
			else
				SetType(op2->Type());
			}
		}
	}

IntrusivePtr<Val> CondExpr::Eval(Frame* f) const
	{
	if ( ! is_vector(op1.get()) )
		{
		// Scalar case
		auto false_eval = op1->Eval(f)->IsZero();
		return (false_eval ? op3 : op2)->Eval(f);
		}

	// Vector case: no mixed scalar/vector cases allowed
	auto v1 = op1->Eval(f);

	if ( ! v1 )
		return nullptr;

	auto v2 = op2->Eval(f);

	if ( ! v2 )
		return nullptr;

	auto v3 = op3->Eval(f);

	if ( ! v3 )
		return nullptr;

	VectorVal* cond = v1->AsVectorVal();
	VectorVal* a = v2->AsVectorVal();
	VectorVal* b = v3->AsVectorVal();

	if ( cond->Size() != a->Size() || a->Size() != b->Size() )
		{
		RuntimeError("vectors in conditional expression have different sizes");
		return nullptr;
		}

	auto result = make_intrusive<VectorVal>(Type()->AsVectorType());
	result->Resize(cond->Size());

	for ( unsigned int i = 0; i < cond->Size(); ++i )
		{
		auto local_cond = cond->Lookup(i);

		if ( local_cond )
			{
			auto v = local_cond->IsZero() ? b->Lookup(i) : a->Lookup(i);
			result->Assign(i, v ? v.release() : nullptr);
			}
		else
			result->Assign(i, 0);
		}

	return result;
	}

bool CondExpr::IsPure() const
	{
	return op1->IsPure() && op2->IsPure() && op3->IsPure();
	}

bool CondExpr::IsReduced(Reducer* c) const
	{
	return NonReduced(this);
	}

bool CondExpr::HasReducedOps(Reducer* c) const
	{
	return op1->IsSingleton(c) && op2->IsSingleton(c) && op3->IsSingleton(c) &&
		! op1->IsConst();
	}

bool CondExpr::WillTransform(Reducer* c) const
	{
	return ! HasReducedOps(c);
	}

Expr* CondExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( c->Optimizing() )
		{
		op1 = c->UpdateExpr(op1);
		op2 = c->UpdateExpr(op2);
		op3 = c->UpdateExpr(op3);
		}

	IntrusivePtr<Stmt> op1_red_stmt;
	op1 = {AdoptRef{}, op1->ReduceToSingleton(c, op1_red_stmt)};

	if ( op1->IsConst() )
		{
		Expr* res;
		if ( op1->AsConstExpr()->Value()->IsOne() )
			res = op2->ReduceToSingleton(c, red_stmt);
		else
			res = op3->ReduceToSingleton(c, red_stmt);

		red_stmt = MergeStmts(op1_red_stmt, red_stmt);

		return res;
		}

	if ( same_singletons(op2, op3) )
		{
		if ( op1->HasNoSideEffects() )
			{
			if ( op1->Tag() != EXPR_CONST &&
			     op1->Tag() != EXPR_NAME )
				{
				op1 = {AdoptRef{},
					op1->AssignToTemporary(c, red_stmt)};
				}
			}

		red_stmt = MergeStmts(op1_red_stmt, red_stmt);

		return op2->Ref();
		}

	if ( c->Optimizing() )
		return this->Ref();

	red_stmt = ReduceToSingletons(c);

	IntrusivePtr<Stmt> assign_stmt;
	auto res = AssignToTemporary(c, assign_stmt);

	red_stmt = MergeStmts(op1_red_stmt, red_stmt, assign_stmt);

	return TransformMe(res, c, red_stmt);
	}

Expr* CondExpr::Inline(Inliner* inl)
	{
	op1 = {NewRef{}, op1->Inline(inl)};
	op2 = {NewRef{}, op2->Inline(inl)};
	op3 = {NewRef{}, op3->Inline(inl)};

	return this->Ref();
	}

IntrusivePtr<Stmt> CondExpr::ReduceToSingletons(Reducer* c)
	{
	IntrusivePtr<Stmt> red1_stmt;
	if ( ! op1->IsSingleton(c) )
		op1 = {AdoptRef{}, op1->ReduceToSingleton(c, red1_stmt)};

	IntrusivePtr<Stmt> red2_stmt;
	if ( ! op2->IsSingleton(c) )
		op2 = {AdoptRef{}, op2->ReduceToSingleton(c, red2_stmt)};

	IntrusivePtr<Stmt> red3_stmt;
	if ( ! op3->IsSingleton(c) )
		op3 = {AdoptRef{}, op3->ReduceToSingleton(c, red3_stmt)};

	IntrusivePtr<Stmt> if_else;

	if ( red2_stmt || red3_stmt )
		{
		if ( ! red2_stmt )
			red2_stmt = make_intrusive<NullStmt>();
		if ( ! red3_stmt )
			red3_stmt = make_intrusive<NullStmt>();

		if_else = {AdoptRef{}, new IfStmt(op1->Duplicate(),
							red2_stmt, red3_stmt)};
		}

	return MergeStmts(red1_stmt, if_else);
	}

IntrusivePtr<Expr> CondExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	auto op3_d = op3->Duplicate();
	return SetSucc(new CondExpr(op1_d, op2_d, op3_d));
	}

TraversalCode CondExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this, Op1());
	HANDLE_TC_EXPR_PRE(tc);

	tc = op1->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = op2->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = op3->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

void CondExpr::ExprDescribe(ODesc* d) const
	{
	op1->Describe(d);
	d->AddSP(" ?");
	op2->Describe(d);
	d->AddSP(" :");
	op3->Describe(d);
	}

RefExpr::RefExpr(IntrusivePtr<Expr> arg_op)
	: UnaryExpr(EXPR_REF, std::move(arg_op))
	{
	if ( IsError() )
		return;

	if ( ! ::is_assignable(op->Type().get()) )
		ExprError("illegal assignment target");
	else
		SetType(op->Type());
	}

IntrusivePtr<Expr> RefExpr::MakeLvalue()
	{
	return {NewRef{}, this};
	}

void RefExpr::Assign(Frame* f, IntrusivePtr<Val> v)
	{
	op->Assign(f, std::move(v));
	}

bool RefExpr::IsReduced(Reducer* c) const
	{
	if ( op->Tag() == EXPR_NAME )
		return op->IsReduced(c);

	return NonReduced(this);
	}

bool RefExpr::HasReducedOps(Reducer* c) const
	{
	switch ( op->Tag() ) {
	case EXPR_NAME:
		return op->IsReduced(c);

	case EXPR_FIELD:
		return op->AsFieldExpr()->Op()->IsReduced(c);

	case EXPR_INDEX:
		{
		auto ind = op->AsIndexExpr();
		return ind->Op1()->IsReduced(c) && ind->Op2()->IsReduced(c);
		}

	case EXPR_LIST:
		return op->IsReduced(c);

	default:
		Internal("bad operand in RefExpr::IsReduced");
		return true;
	}
	}

bool RefExpr::WillTransform(Reducer* c) const
	{
	return op->Tag() != EXPR_NAME;
	}

Expr* RefExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( op->Tag() == EXPR_NAME )
		op = {AdoptRef{}, op->Reduce(c, red_stmt)};
	else
		op = {AdoptRef{}, AssignToTemporary(c, red_stmt)};

	return this->Ref();
	}

IntrusivePtr<Stmt> RefExpr::ReduceToLHS(Reducer* c)
	{
	if ( op->Tag() == EXPR_NAME )
		{
		IntrusivePtr<Stmt> red_stmt;
		op = {AdoptRef{}, op->Reduce(c, red_stmt)};
		return red_stmt;
		}

	auto red_stmt1 = op->ReduceToSingletons(c);
	auto op_ref = make_intrusive<RefExpr>(op);

	IntrusivePtr<Stmt> red_stmt2;
	op = {AdoptRef{}, AssignToTemporary(op_ref.get(), c, red_stmt2)};

	return MergeStmts(red_stmt1, red_stmt2);
	}

IntrusivePtr<Expr> RefExpr::Duplicate()
	{
	return SetSucc(new RefExpr(op->Duplicate()));
	}

AssignExpr::AssignExpr(IntrusivePtr<Expr> arg_op1, IntrusivePtr<Expr> arg_op2,
                       bool arg_is_init, IntrusivePtr<Val> arg_val,
                       attr_list* arg_attrs, bool typecheck)
	: BinaryExpr(EXPR_ASSIGN, arg_is_init ?
	             std::move(arg_op1) : arg_op1->MakeLvalue(),
	             std::move(arg_op2))
	{
	val = 0;
	is_init = arg_is_init;
	is_temp = false;

	if ( IsError() )
		return;

	SetType({NewRef{}, arg_val ? arg_val->Type() : op1->Type().get()});

	if ( is_init )
		{
		SetLocationInfo(op1->GetLocationInfo(),
				op2->GetLocationInfo());
		return;
		}

	if ( typecheck )
		// We discard the status from TypeCheck since it has already
		// generated error messages.
		(void) TypeCheck(arg_attrs);

	val = std::move(arg_val);

	SetLocationInfo(op1->GetLocationInfo(), op2->GetLocationInfo());
	}

bool AssignExpr::TypeCheck(attr_list* attrs)
	{
	TypeTag bt1 = op1->Type()->Tag();
	TypeTag bt2 = op2->Type()->Tag();

	if ( bt1 == TYPE_LIST && bt2 == TYPE_ANY )
		// This is ok because we cannot explicitly declare lists on
		// the script level.
		return true;

	// This should be one of them, but not both (i.e. XOR)
	if ( ((bt1 == TYPE_ENUM) ^ (bt2 == TYPE_ENUM)) )
		{
		ExprError("can't convert to/from enumerated type");
		return false;
		}

	if ( IsArithmetic(bt1) )
		return TypeCheckArithmetics(bt1, bt2);

	if ( bt1 == TYPE_TIME && IsArithmetic(bt2) && op2->IsZero() )
		{ // Allow assignments to zero as a special case.
		op2 = make_intrusive<ArithCoerceExpr>(std::move(op2), bt1);
		return true;
		}

	if ( bt1 == TYPE_TABLE && bt2 == bt1 &&
	     op2->Type()->AsTableType()->IsUnspecifiedTable() )
		{
		op2 = make_intrusive<TableCoerceExpr>(std::move(op2),
		        IntrusivePtr{NewRef{}, op1->Type()->AsTableType()});
		return true;
		}

	if ( bt1 == TYPE_TABLE && op2->Tag() == EXPR_LIST )
		{
		attr_list* attr_copy = 0;

		if ( attrs )
			{
			attr_copy = new attr_list(attrs->length());
			std::copy(attrs->begin(), attrs->end(), std::back_inserter(*attr_copy));
			}

		bool empty_list_assignment = (op2->AsListExpr()->Exprs().empty());

		if ( op1->Type()->IsSet() )
			op2 = make_intrusive<SetConstructorExpr>(
			        IntrusivePtr{NewRef{}, op2->AsListExpr()}, attr_copy);
		else
			op2 = make_intrusive<TableConstructorExpr>(
			        IntrusivePtr{NewRef{}, op2->AsListExpr()}, attr_copy);

		if ( ! empty_list_assignment &&
		     ! same_type(op1->Type(), op2->Type()) )
			{
			if ( op1->Type()->IsSet() )
				ExprError("set type mismatch in assignment");
			else
				ExprError("table type mismatch in assignment");

			return false;
			}

		return true;
		}

	if ( bt1 == TYPE_VECTOR )
		{
		if ( bt2 == bt1 && op2->Type()->AsVectorType()->IsUnspecifiedVector() )
			{
			op2 = make_intrusive<VectorCoerceExpr>(std::move(op2),
			        IntrusivePtr{NewRef{}, op1->Type()->AsVectorType()});
			return true;
			}

		if ( op2->Tag() == EXPR_LIST )
			{
			op2 = make_intrusive<VectorConstructorExpr>(
			        IntrusivePtr{AdoptRef{}, op2.release()->AsListExpr()},
			        op1->Type());
			return true;
			}
		}

	if ( op1->Type()->Tag() == TYPE_RECORD &&
	     op2->Type()->Tag() == TYPE_RECORD )
		{
		if ( same_type(op1->Type(), op2->Type()) )
			{
			RecordType* rt1 = op1->Type()->AsRecordType();
			RecordType* rt2 = op2->Type()->AsRecordType();

			// Make sure the attributes match as well.
			for ( int i = 0; i < rt1->NumFields(); ++i )
				{
				const TypeDecl* td1 = rt1->FieldDecl(i);
				const TypeDecl* td2 = rt2->FieldDecl(i);

				if ( same_attrs(td1->attrs.get(), td2->attrs.get()) )
					// Everything matches.
					return true;
				}
			}

		// Need to coerce.
		op2 = make_intrusive<RecordCoerceExpr>(std::move(op2),
		        IntrusivePtr{NewRef{}, op1->Type()->AsRecordType()});
		return true;
		}

	if ( ! same_type(op1->Type(), op2->Type()) )
		{
		if ( bt1 == TYPE_TABLE && bt2 == TYPE_TABLE )
			{
			if ( op2->Tag() == EXPR_SET_CONSTRUCTOR )
				{
				// Some elements in constructor list must not match, see if
				// we can create a new constructor now that the expected type
				// of LHS is known and let it do coercions where possible.
				SetConstructorExpr* sce = dynamic_cast<SetConstructorExpr*>(op2.get());

				if ( ! sce )
					{
					ExprError("Failed typecast to SetConstructorExpr");
					return false;
					}

				ListExpr* ctor_list = dynamic_cast<ListExpr*>(sce->Op());

				if ( ! ctor_list )
					{
					ExprError("Failed typecast to ListExpr");
					return false;
					}

				attr_list* attr_copy = 0;

				if ( sce->Attrs() )
					{
					attr_list* a = sce->Attrs()->Attrs();
					attrs = new attr_list(a->length());
					std::copy(a->begin(), a->end(), std::back_inserter(*attrs));
					}

				int errors_before = reporter->Errors();
				op2 = make_intrusive<SetConstructorExpr>(
				        IntrusivePtr{NewRef{}, ctor_list}, attr_copy,
				        op1->Type());
				int errors_after = reporter->Errors();

				if ( errors_after > errors_before )
					{
					ExprError("type clash in assignment");
					return false;
					}

				return true;
				}
			}

		ExprError("type clash in assignment");
		return false;
		}

	return true;
	}

bool AssignExpr::TypeCheckArithmetics(TypeTag bt1, TypeTag bt2)
	{
	if ( ! IsArithmetic(bt2) )
		{
		ExprError(fmt("assignment of non-arithmetic value to arithmetic (%s/%s)",
				type_name(bt1), type_name(bt2)));
		return false;
		}

	if ( bt1 == TYPE_DOUBLE )
		{
		PromoteOps(TYPE_DOUBLE);
		return true;
		}

	if ( bt2 == TYPE_DOUBLE )
		{
		Warn("dangerous assignment of double to integral");
		op2 = make_intrusive<ArithCoerceExpr>(std::move(op2), bt1);
		bt2 = op2->Type()->Tag();
		}

	if ( bt1 == TYPE_INT )
		PromoteOps(TYPE_INT);
	else
		{
		if ( bt2 == TYPE_INT )
			{
			Warn("dangerous assignment of integer to count");
			op2 = make_intrusive<ArithCoerceExpr>(std::move(op2), bt1);
			}

		// Assignment of count to counter or vice
		// versa is allowed, and requires no
		// coercion.
		}

	return true;
	}


IntrusivePtr<Val> AssignExpr::Eval(Frame* f) const
	{
	if ( is_init )
		{
		RuntimeError("illegal assignment in initialization");
		return nullptr;
		}

	if ( auto v = op2->Eval(f) )
		{
		SeatBelts(v->Type(), op2->Type());

		op1->Assign(f, v);

		if ( val )
			return val;

		return v;
		}
	else
		return nullptr;
	}

IntrusivePtr<BroType> AssignExpr::InitType() const
	{
	if ( op1->Tag() != EXPR_LIST )
		{
		Error("bad initializer");
		return nullptr;
		}

	auto tl = op1->Type();
	if ( tl->Tag() != TYPE_LIST )
		Internal("inconsistent list expr in AssignExpr::InitType");

	return make_intrusive<TableType>(IntrusivePtr{NewRef{}, tl->AsTypeList()},
	                                 op2->Type());
	}

void AssignExpr::EvalIntoAggregate(const BroType* t, Val* aggr, Frame* f) const
	{
	if ( IsError() )
		return;

	TypeDecl td(nullptr, nullptr);

	if ( IsRecordElement(&td) )
		{
		if ( t->Tag() != TYPE_RECORD )
			{
			RuntimeError("not a record initializer");
			return;
			}

		const RecordType* rt = t->AsRecordType();
		int field = rt->FieldOffset(td.id);

		if ( field < 0 )
			{
			RuntimeError("no such field");
			return;
			}

		RecordVal* aggr_r = aggr->AsRecordVal();

		auto v = op2->Eval(f);

		if ( v )
			aggr_r->Assign(field, std::move(v));

		return;
		}

	if ( op1->Tag() != EXPR_LIST )
		RuntimeError("bad table insertion");

	TableVal* tv = aggr->AsTableVal();

	auto index = op1->Eval(f);
	auto v = check_and_promote(op2->Eval(f), t->YieldType(), true);

	if ( ! index || ! v )
		return;

	if ( ! tv->Assign(index.get(), std::move(v)) )
		RuntimeError("type clash in table assignment");
	}

IntrusivePtr<Val> AssignExpr::InitVal(const BroType* t, IntrusivePtr<Val> aggr) const
	{
	if ( ! aggr )
		{
		Error("assignment in initialization");
		return nullptr;
		}

	if ( IsError() )
		return nullptr;

	TypeDecl td(nullptr, nullptr);

	if ( IsRecordElement(&td) )
		{
		if ( t->Tag() != TYPE_RECORD )
			{
			Error("not a record initializer", t);
			return nullptr;
			}

		const RecordType* rt = t->AsRecordType();
		int field = rt->FieldOffset(td.id);

		if ( field < 0 )
			{
			Error("no such field");
			return nullptr;
			}

		if ( aggr->Type()->Tag() != TYPE_RECORD )
			Internal("bad aggregate in AssignExpr::InitVal");

		RecordVal* aggr_r = aggr->AsRecordVal();

		auto v = op2->InitVal(rt->FieldType(td.id), nullptr);

		if ( ! v )
			return nullptr;

		aggr_r->Assign(field, v);
		return v;
		}

	else if ( op1->Tag() == EXPR_LIST )
		{
		if ( t->Tag() != TYPE_TABLE )
			{
			Error("not a table initialization", t);
			return nullptr;
			}

		if ( aggr->Type()->Tag() != TYPE_TABLE )
			Internal("bad aggregate in AssignExpr::InitVal");

		 // TODO: implement safer IntrusivePtr casts
		IntrusivePtr<TableVal> tv{NewRef{}, aggr->AsTableVal()};
		const TableType* tt = tv->Type()->AsTableType();
		const BroType* yt = tv->Type()->YieldType();

		auto index = op1->InitVal(tt->Indices(), nullptr);
		auto v = op2->InitVal(yt, nullptr);

		if ( ! index || ! v )
			return nullptr;

		if ( ! tv->ExpandAndInit(std::move(index), std::move(v)) )
			return nullptr;

		return tv;
		}

	else
		{
		Error("illegal initializer");
		return nullptr;
		}
	}

bool AssignExpr::IsRecordElement(TypeDecl* td) const
	{
	if ( op1->Tag() == EXPR_NAME )
		{
		if ( td )
			{
			const NameExpr* n = (const NameExpr*) op1.get();
			td->type = op2->Type();
			td->id = copy_string(n->Id()->Name());
			}

		return true;
		}

	return false;
	}

bool AssignExpr::IsPure() const
	{
	return false;
	}

bool AssignExpr::HasNoSideEffects() const
	{
	return false;
	}

bool AssignExpr::IsReduced(Reducer* c) const
	{
	if ( op2->Tag() == EXPR_ASSIGN )
		// Cascaded assignments are never reduced.
		return false;

	auto lhs_is_any = op1->Type()->Tag() == TYPE_ANY;
	auto rhs_is_any = op2->Type()->Tag() == TYPE_ANY;

	if ( lhs_is_any != rhs_is_any && op2->Tag() != EXPR_CONST )
		return NonReduced(this);

	auto t1 = op1->Tag();

	if ( t1 == EXPR_REF &&
	     op2->HasConstantOps() && op2->Tag() != EXPR_TO_ANY_COERCE )
		// We are not reduced because we should instead
		// be folded.
		return NonReduced(this);

	if ( IsTemp() )
		return true;

	if ( ! op2->HasReducedOps(c) )
		return false;

	if ( op1->IsSingleton(c) )
		return true;

	if ( op1->Tag() == EXPR_REF )
		return op1->AsRefExpr()->IsReduced(c);

	return NonReduced(this);
	}

bool AssignExpr::HasReducedOps(Reducer* c) const
	{
	return op1->IsReduced(c) && op2->IsSingleton(c);
	}

Expr* AssignExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	// Yields a fully reduced assignment expression.

	if ( c->Optimizing() )
		{
		// Don't update the LHS, it's already in reduced form
		// and it doesn't make sense to expand aliases or such.
		op2 = c->UpdateExpr(op2);
		return this->Ref();
		}

	if ( IsTemp() )
		// These are generated for reduced expressions.
		return this->Ref();

	auto lhs_is_any = op1->Type()->Tag() == TYPE_ANY;
	auto rhs_is_any = op2->Type()->Tag() == TYPE_ANY;

	IntrusivePtr<Stmt> rhs_reduce;

	if ( lhs_is_any != rhs_is_any )
		{
		IntrusivePtr<Expr> red_rhs =
			{AdoptRef{}, op2->ReduceToSingleton(c, rhs_reduce)};

		if ( lhs_is_any )
			{
			if ( red_rhs->Tag() == EXPR_CONST )
				op2 = red_rhs;
			else
				op2 = make_intrusive<CoerceToAnyExpr>(red_rhs);
			}
		else
			op2 = make_intrusive<CoerceFromAnyExpr>(red_rhs,
								op1->Type());
		}

	auto lhs_ref = op1->AsRefExpr();
	auto lhs_expr = lhs_ref->Op();

	if ( lhs_expr->Tag() == EXPR_INDEX )
		{
		auto ind_e = lhs_expr->AsIndexExpr();

		IntrusivePtr<Stmt> ind1_stmt;
		IntrusivePtr<Stmt> ind2_stmt;
		IntrusivePtr<Stmt> rhs_stmt;

		IntrusivePtr<Expr> ind1_e =
			{AdoptRef{}, ind_e->Op1()->Reduce(c, ind1_stmt)};
		IntrusivePtr<Expr> ind2_e 
			{AdoptRef{}, ind_e->Op2()->Reduce(c, ind2_stmt)};
		IntrusivePtr<Expr> rhs_e 
			{AdoptRef{}, op2->Reduce(c, rhs_stmt)};

		red_stmt = MergeStmts(MergeStmts(rhs_reduce, ind1_stmt),
					ind2_stmt, rhs_stmt);

		auto index_assign = new IndexAssignExpr(ind1_e, ind2_e, rhs_e);
		return TransformMe(index_assign, c, red_stmt);
		}

	if ( lhs_expr->Tag() == EXPR_FIELD )
		{
		auto field_e = lhs_expr->AsFieldExpr();

		IntrusivePtr<Stmt> lhs_stmt;
		IntrusivePtr<Stmt> rhs_stmt;

		IntrusivePtr<Expr> lhs_e =
			{AdoptRef{}, field_e->Op()->Reduce(c, lhs_stmt)};
		IntrusivePtr<Expr> rhs_e =
			{AdoptRef{}, op2->ReduceToFieldAssignment(c, rhs_stmt)};

		red_stmt = MergeStmts(rhs_reduce, lhs_stmt, rhs_stmt);

		auto field_name = field_e->FieldName();
		auto field = field_e->Field();
		auto field_assign =
			new FieldLHSAssignExpr(lhs_e, rhs_e, field_name, field);

		return TransformMe(field_assign, c, red_stmt);
		}

	if ( lhs_expr->Tag() == EXPR_LIST )
		{
		auto lhs_list = lhs_expr->AsListExpr()->Exprs();

		IntrusivePtr<Stmt> rhs_stmt;
		IntrusivePtr<Expr> rhs_e =
			{AdoptRef{}, op2->Reduce(c, rhs_stmt)};

		auto len = lhs_list.length();
		auto check_stmt = make_intrusive<CheckAnyLenStmt>(rhs_e, len);

		red_stmt = MergeStmts(rhs_reduce, rhs_stmt, check_stmt);

		loop_over_list(lhs_list, i)
			{
			auto rhs_dup = rhs_e->Duplicate();
			auto rhs = make_intrusive<AnyIndexExpr>(rhs_dup, i);
			IntrusivePtr<Expr> lhs = {NewRef{}, lhs_list[i]};
			auto assign = make_intrusive<AssignExpr>(lhs, rhs,
						false, nullptr, nullptr, false);
			auto assign_stmt = make_intrusive<ExprStmt>(assign);
			red_stmt = MergeStmts(red_stmt, assign_stmt);
			}

		auto nop = new NopExpr();
		return TransformMe(nop, c, red_stmt);
		}

	if ( op2->WillTransform(c) )
		{
		IntrusivePtr<Stmt> xform_stmt;
		op2 = {AdoptRef{}, op2->ReduceToSingleton(c, xform_stmt)};
		red_stmt = MergeStmts(rhs_reduce, xform_stmt);
		return this->Ref();
		}

	red_stmt = op2->ReduceToSingletons(c);

	if ( op2->HasConstantOps() && op2->Tag() != EXPR_TO_ANY_COERCE )
		op2 = make_intrusive<ConstExpr>(op2->Eval(nullptr));

	// Check once again for transformation, this time made possible
	// because the operands have been reduced.  We don't simply
	// always first reduce the operands, because for expressions
	// like && and ||, that's incorrect.

	if ( op2->WillTransform(c) )
		{
		IntrusivePtr<Stmt> xform_stmt;
		op2 = {AdoptRef{}, op2->ReduceToSingleton(c, xform_stmt)};
		red_stmt = MergeStmts(rhs_reduce, red_stmt, xform_stmt);
		return this->Ref();
		}

	IntrusivePtr<Stmt> lhs_stmt = lhs_ref->ReduceToLHS(c);
	IntrusivePtr<Stmt> rhs_stmt = op2->ReduceToSingletons(c);

	red_stmt = MergeStmts(MergeStmts(rhs_reduce, red_stmt),
				lhs_stmt, rhs_stmt);

	return this->Ref();
	}

Expr* AssignExpr::ReduceToSingleton(Reducer* c,
					IntrusivePtr<Stmt>& red_stmt)
	{
	// Yields a statement performing the assignment and for the
	// expression the LHS (but turned into an RHS).
	if ( op1->Tag() != EXPR_REF )
		Internal("Confusion in AssignExpr::ReduceToSingleton");

	IntrusivePtr<Expr> assign_expr = Duplicate();
	red_stmt = make_intrusive<ExprStmt>(assign_expr);
	red_stmt = {AdoptRef{}, red_stmt->Reduce(c)};

	return op1->AsRefExpr()->Op()->Ref();
	}

const CompiledStmt AssignExpr::Compile(Compiler* c) const
	{
	auto lhs = op1->AsRefExpr()->GetOp1()->AsNameExpr();
	auto s = DoCompile(c, lhs);
	auto lhs_id = lhs->Id();

	if ( lhs_id->IsGlobal() )
		// Give the compiler notice that a global may require
		// updating.
		return c->AssignedToGlobal(lhs_id);
	else
		return s;
	}

const CompiledStmt AssignExpr::DoCompile(Compiler* c, const NameExpr* lhs) const
	{
	auto lt = lhs->Type().get();
	auto rhs = op2.get();
	auto r1 = rhs->GetOp1();

	if ( rhs->Tag() == EXPR_INDEX &&
	     (r1->Tag() == EXPR_NAME || r1->Tag() == EXPR_CONST) )
		return CompileAssignToIndex(c, lhs, rhs->AsIndexExpr());

	switch ( rhs->Tag() ) {
#include "CompilerOpsDirectDefs.h"
	}

	auto rt = rhs->Type();

	auto r2 = rhs->GetOp2();
	auto r3 = rhs->GetOp3();

	if ( rhs->Tag() == EXPR_LAMBDA )
		{
		// ###
		// Error("lambda expressions not supported for compiling");
		return c->ErrorStmt();
		}

	if ( rhs->Tag() == EXPR_NAME )
		return c->AssignXV(lhs, rhs->AsNameExpr());

	if ( rhs->Tag() == EXPR_CONST )
		return c->AssignXC(lhs, rhs->AsConstExpr());

	if ( rhs->Tag() == EXPR_IN && r1->Tag() == EXPR_LIST )
		{
		// r2 can be a constant due to propagating "const"
		// globals, for example.
		if ( r2->Tag() == EXPR_NAME )
			{
			auto r2n = r2->AsNameExpr();

			if ( r2->Type()->Tag() == TYPE_TABLE )
				return c->L_In_TVLV(lhs, r1->AsListExpr(), r2n);
			return c->L_In_VecVLV(lhs, r1->AsListExpr(), r2n);
			}

		auto r2c = r2->AsConstExpr();

		if ( r2->Type()->Tag() == TYPE_TABLE )
			return c->L_In_TVLC(lhs, r1->AsListExpr(), r2c);
		return c->L_In_VecVLC(lhs, r1->AsListExpr(), r2c);
		}

	if ( rhs->Tag() == EXPR_ANY_INDEX )
		return c->AnyIndexVVi(lhs, r1->AsNameExpr(),
					rhs->AsAnyIndexExpr()->Index());

	if ( rhs->Tag() == EXPR_COND && r2->IsConst() && r3->IsConst() )
		{
		// Split into two statement, given we don't support
		// two constants in a single statement.
		auto n1 = r1->AsNameExpr();
		auto c2 = r2->AsConstExpr();
		auto c3 = r3->AsConstExpr();
		(void) c->CondC1VVC(lhs, n1, c2);
		return c->CondC2VVC(lhs, n1, c3);
		}

	if ( r1 && r2 )
		{
		auto v1 = IsVector(r1->Type()->Tag());
		auto v2 = IsVector(r2->Type()->Tag());

		if ( v1 != v2 && rhs->Tag() != EXPR_IN )
			{
			Error("deprecated mixed vector/scalar operation not supported for compiling");
			return c->ErrorStmt();
			}
		}

	if ( r1 && r1->IsConst() )
#include "CompilerOpsExprsDefsC1.h"

	else if ( r2 && r2->IsConst() )
#include "CompilerOpsExprsDefsC2.h"

	else if ( r3 && r3->IsConst() )
#include "CompilerOpsExprsDefsC3.h"

	else
#include "CompilerOpsExprsDefsV.h"
	}

const CompiledStmt AssignExpr::CompileAssignToIndex(Compiler* c,
						const NameExpr* lhs,
						const IndexExpr* rhs) const
	{
	auto aggr_ptr = rhs->GetOp1();
	auto const_aggr = aggr_ptr->Tag() == EXPR_CONST;

	auto indexes_expr = rhs->GetOp2()->AsListExpr();
	auto indexes = indexes_expr->Exprs();

	auto n = const_aggr ? nullptr : aggr_ptr->AsNameExpr();
	auto con = const_aggr ? aggr_ptr->AsConstExpr() : nullptr;

	if ( indexes.length() == 1 && indexes[0]->Type()->Tag() == TYPE_VECTOR )
		{
		auto index1 = indexes[0];
		if ( index1->Tag() == EXPR_CONST )
			{
			Error("constant vector indexes not supported for compiling");
			return c->ErrorStmt();
			}

		auto index = index1->AsNameExpr();
		auto ind_t = index->Type()->AsVectorType();

		if ( IsBool(ind_t->YieldType()->Tag()) )
			return const_aggr ?
				c->IndexVecBoolSelectVCV(lhs, con, index) :
				c->IndexVecBoolSelectVVV(lhs, n, index);

		return const_aggr ? c->IndexVecIntSelectVCV(lhs, con, index) :
					c->IndexVecIntSelectVVV(lhs, n, index);
		}

	return const_aggr ? c->IndexVCL(lhs, con, indexes_expr) :
				c->IndexVVL(lhs, n, indexes_expr);
	}

IntrusivePtr<Expr> AssignExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	return SetSucc(new AssignExpr(op1_d, op2_d, is_init, val));
	}

IndexAssignExpr::IndexAssignExpr(IntrusivePtr<Expr> arg_op1,
					IntrusivePtr<Expr> arg_op2,
					IntrusivePtr<Expr> arg_op3)
: BinaryExpr(EXPR_INDEX_ASSIGN, std::move(arg_op1), std::move(arg_op2))
	{
	op3 = arg_op3;
	SetType(op3->Type());
	}

IntrusivePtr<Val> IndexAssignExpr::Eval(Frame* f) const
	{
	auto v1 = op1->Eval(f);
	auto v2 = op2->Eval(f);
	auto v3 = op3->Eval(f);

	AssignToIndex(v1, v2, v3);

	return nullptr;
	}

bool IndexAssignExpr::IsReduced(Reducer* c) const
	{
	// op2 is a ListExpr, not a singleton expression.
	ASSERT(op1->IsSingleton(c) && op2->IsReduced(c) && op3->IsSingleton(c));
	return true;
	}

bool IndexAssignExpr::HasReducedOps(Reducer* c) const
	{
	return true;
	}

Expr* IndexAssignExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( c->Optimizing() )
		{
		op1 = c->UpdateExpr(op1);
		op2 = c->UpdateExpr(op2);
		op3 = c->UpdateExpr(op3);
		}

	return this->Ref();
	}

Expr* IndexAssignExpr::ReduceToSingleton(Reducer* c,
					IntrusivePtr<Stmt>& red_stmt)
	{
	// Yields a statement performing the assignment and for the
	// expression the LHS (but turned into an RHS).
	if ( op1->Tag() != EXPR_NAME )
		Internal("Confusion in IndexAssignExpr::ReduceToSingleton");

	IntrusivePtr<Stmt> op1_red_stmt;
	op1 = {AdoptRef{}, op1->Reduce(c, op1_red_stmt)};

	IntrusivePtr<Expr> assign_expr = Duplicate();
	auto assign_stmt = make_intrusive<ExprStmt>(assign_expr);

	IntrusivePtr<ListExpr> index = {NewRef{}, op2->AsListExpr()};
	auto res = new IndexExpr(GetOp1(), index, false);
	auto final_res = res->ReduceToSingleton(c, red_stmt);

	red_stmt = MergeStmts(op1_red_stmt, assign_stmt, red_stmt);

	return final_res;
	}

const CompiledStmt IndexAssignExpr::Compile(Compiler* c) const
	{
	auto t = op1->Type()->Tag();

	if ( t == TYPE_VECTOR )
		return c->AssignVecElems(this);

	ASSERT(t == TYPE_TABLE);
	return c->AssignTableElem(this);
	}

IntrusivePtr<Expr> IndexAssignExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	auto op3_d = op3->Duplicate();
	return SetSucc(new IndexAssignExpr(op1_d, op2_d, op3_d));
	}

TraversalCode IndexAssignExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this, Op1());
	HANDLE_TC_EXPR_PRE(tc);

	tc = op1->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = op2->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = op3->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

void IndexAssignExpr::ExprDescribe(ODesc* d) const
	{
	op1->Describe(d);
	if ( d->IsReadable() )
		d->Add("[");

	op2->Describe(d);
	if ( d->IsReadable() )
		{
		d->Add("]");
		d->Add(" []= ");
		}

	op3->Describe(d);
	}


IndexSliceAssignExpr::IndexSliceAssignExpr(IntrusivePtr<Expr> op1,
                                           IntrusivePtr<Expr> op2, bool is_init)
	: AssignExpr(std::move(op1), std::move(op2), is_init)
	{
	}

IntrusivePtr<Val> IndexSliceAssignExpr::Eval(Frame* f) const
	{
	if ( is_init )
		{
		RuntimeError("illegal assignment in initialization");
		return nullptr;
		}

	if ( auto v = op2->Eval(f) )
		op1->Assign(f, std::move(v));

	return nullptr;
	}

const CompiledStmt IndexSliceAssignExpr::Compile(Compiler* c) const
	{
	Internal("IndexSliceAssignExpr was not transformed away");
	}

IntrusivePtr<Expr> IndexSliceAssignExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	return SetSucc(new IndexSliceAssignExpr(op1_d, op2_d, is_init));
	}


IndexExpr::IndexExpr(IntrusivePtr<Expr> arg_op1,
                     IntrusivePtr<ListExpr> arg_op2, bool arg_is_slice)
	: BinaryExpr(EXPR_INDEX, std::move(arg_op1), std::move(arg_op2)),
	  is_slice(arg_is_slice)
	{
	if ( IsError() )
		return;

	if ( is_slice )
		{
		if ( ! IsString(op1->Type()->Tag()) && ! IsVector(op1->Type()->Tag()) )
			ExprError("slice notation indexing only supported for strings and vectors currently");
		}

	else if ( IsString(op1->Type()->Tag()) )
		{
		if ( op2->AsListExpr()->Exprs().length() != 1 )
			ExprError("invalid string index expression");
		}

	if ( IsError() )
		return;

	int match_type = op1->Type()->MatchesIndex(op2->AsListExpr());

	if ( match_type == DOES_NOT_MATCH_INDEX )
		{
		std::string error_msg =
		    fmt("expression with type '%s' is not a type that can be indexed",
		        type_name(op1->Type()->Tag()));
		SetError(error_msg.data());
		}

	else if ( ! op1->Type()->YieldType() )
		{
		if ( IsString(op1->Type()->Tag()) && match_type == MATCHES_INDEX_SCALAR )
			SetType(base_type(TYPE_STRING));
		else
			// It's a set - so indexing it yields void.  We don't
			// directly generate an error message, though, since this
			// expression might be part of an add/delete statement,
			// rather than yielding a value.
			SetType(base_type(TYPE_VOID));
		}

	else if ( match_type == MATCHES_INDEX_SCALAR )
		SetType({NewRef{}, op1->Type()->YieldType()});

	else if ( match_type == MATCHES_INDEX_VECTOR )
		SetType(make_intrusive<VectorType>(IntrusivePtr{NewRef{}, op1->Type()->YieldType()}));

	else
		ExprError("Unknown MatchesIndex() return value");
	}

bool IndexExpr::CanAdd() const
	{
	if ( IsError() )
		return true;	// avoid cascading the error report

	// "add" only allowed if our type is "set".
	return op1->Type()->IsSet();
	}

bool IndexExpr::CanDel() const
	{
	if ( IsError() )
		return true;	// avoid cascading the error report

	return op1->Type()->Tag() == TYPE_TABLE;
	}

void IndexExpr::Add(Frame* f)
	{
	if ( IsError() )
		return;

	auto v1 = op1->Eval(f);

	if ( ! v1 )
		return;

	auto v2 = op2->Eval(f);

	if ( ! v2 )
		return;

	v1->AsTableVal()->Assign(v2.get(), nullptr);
	}

void IndexExpr::Delete(Frame* f)
	{
	if ( IsError() )
		return;

	auto v1 = op1->Eval(f);

	if ( ! v1 )
		return;

	auto v2 = op2->Eval(f);

	if ( ! v2 )
		return;

	v1->AsTableVal()->Delete(v2.get());
	}

IntrusivePtr<Expr> IndexExpr::MakeLvalue()
	{
	if ( IsString(op1->Type()->Tag()) )
		ExprError("cannot assign to string index expression");

	return make_intrusive<RefExpr>(IntrusivePtr{NewRef{}, this});
	}

IntrusivePtr<Val> IndexExpr::Eval(Frame* f) const
	{
	auto v1 = op1->Eval(f);

	if ( ! v1 )
		return nullptr;

	auto v2 = op2->Eval(f);

	if ( ! v2 )
		return nullptr;

	Val* indv = v2->AsListVal()->Index(0);

	if ( is_vector(indv) )
		{
		VectorVal* v_v1 = v1->AsVectorVal();
		VectorVal* v_v2 = indv->AsVectorVal();
		auto vt = Type()->AsVectorType();

		// Booleans select each element (or not).
		if ( IsBool(v_v2->Type()->YieldType()->Tag()) )
			{
			if ( v_v1->Size() != v_v2->Size() )
				{
				RuntimeError("size mismatch, boolean index and vector");
				return nullptr;
				}

			return vector_bool_select(vt, v_v1, v_v2);
			}
		else
			// Elements are indices.
			return vector_int_select(vt, v_v1, v_v2);
		}
	else
		return Fold(v1.get(), v2.get());
	}

IntrusivePtr<Stmt> IndexExpr::ReduceToSingletons(Reducer* c)
	{
	IntrusivePtr<Stmt> red1_stmt;
	if ( ! op1->IsSingleton(c) )
		SetOp1({AdoptRef{}, op1->ReduceToSingleton(c, red1_stmt)});

	IntrusivePtr<Stmt> red2_stmt = op2->ReduceToSingletons(c);

	return MergeStmts(red1_stmt, red2_stmt);
	}

IntrusivePtr<Val> IndexExpr::Fold(Val* v1, Val* v2) const
	{
	if ( IsError() )
		return nullptr;

	IntrusivePtr<Val> v;

	switch ( v1->Type()->Tag() ) {
	case TYPE_VECTOR:
		{
		VectorVal* vect = v1->AsVectorVal();
		const ListVal* lv = v2->AsListVal();

		if ( lv->Length() == 1 )
			v = vect->Lookup(v2);
		else
			{
			auto vt = vect->Type()->AsVectorType();
			v = vector_index(vt, vect, lv);
			}
		}
		break;

	case TYPE_TABLE:
		// I have no idea what this next comment refers to - VP:
		// Then, we jump into the TableVal here.
		v = v1->AsTableVal()->Lookup(v2);
		break;

	case TYPE_STRING:
		{
		const ListVal* lv = v2->AsListVal();
		const BroString* s = v1->AsString();
		BroString* substring = 0;

		if ( lv->Length() == 1 )
			{
			bro_int_t idx = lv->Index(0)->AsInt();

			if ( idx < 0 )
				idx += s->Len();

			// Out-of-range index will return null pointer.
			substring = s->GetSubstring(idx, 1);
			}
		else
			substring = index_string_slice(s, lv);

		return make_intrusive<StringVal>(substring ? substring : new BroString(""));
		}

	default:
		RuntimeError("type cannot be indexed");
		break;
	}

	if ( v )
		return v;

	RuntimeError("no such index");
	return nullptr;
	}

IntrusivePtr<Expr> IndexExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_l = op2->Duplicate()->AsListExprPtr();
	return SetSucc(new IndexExpr(op1_d, op2_l, is_slice));
	}

IntrusivePtr<VectorVal> vector_bool_select(VectorType* vt, const VectorVal* v1,
					const VectorVal* v2)
	{
	auto v_result = make_intrusive<VectorVal>(vt);

	for ( unsigned int i = 0; i < v2->Size(); ++i )
		{
		if ( v2->Lookup(i)->AsBool() )
			{
			auto a = v1->Lookup(i);
			v_result->Assign(v_result->Size() + 1,
						a ? a->Ref() : nullptr);
			}
		}

	return v_result;
	}

IntrusivePtr<VectorVal> vector_int_select(VectorType* vt, const VectorVal* v1,
					const VectorVal* v2)
	{
	auto v_result = make_intrusive<VectorVal>(vt);

	// The elements are indices.
	//
	// ### Should handle negative indices here like S does, i.e.,
	// by excluding those elements.  Probably only do this if *all*
	// are negative.
	v_result->Resize(v2->Size());
	for ( unsigned int i = 0; i < v2->Size(); ++i )
		{
		auto a = v1->Lookup(v2->Lookup(i)->CoerceToInt());
		v_result->Assign(i, a ? a->Ref() : nullptr);
		}

	return v_result;
	}

IntrusivePtr<VectorVal> vector_index(VectorType* vt, const VectorVal* vect,
				const ListVal* lv)
	{
	size_t len = vect->Size();
	auto result = make_intrusive<VectorVal>(vt);

	bro_int_t first = get_slice_index(lv->Index(0)->CoerceToInt(), len);
	bro_int_t last = get_slice_index(lv->Index(1)->CoerceToInt(), len);
	bro_int_t sub_length = last - first;

	if ( sub_length >= 0 )
		{
		result->Resize(sub_length);

		for ( int idx = first; idx < last; idx++ )
			{
			auto a = vect->Lookup(idx);
			result->Assign(idx - first, a ? a->Ref() : nullptr);
			}
		}

	return result;
	}

BroString* index_string_slice(const BroString* s, const ListVal* lv)
	{
	int len = s->Len();

	bro_int_t first = get_slice_index(lv->Index(0)->AsInt(), len);
	bro_int_t last = get_slice_index(lv->Index(1)->AsInt(), len);
	bro_int_t substring_len = last - first;

	if ( substring_len < 0 )
		return 0;
	else
		return s->GetSubstring(first, substring_len);
	}

void IndexExpr::Assign(Frame* f, IntrusivePtr<Val> v)
	{
	if ( IsError() )
		return;

	auto v1 = op1->Eval(f);
	auto v2 = op2->Eval(f);

	AssignToIndex(v1, v2, v);
	}

bool IndexExpr::HasReducedOps(Reducer* c) const
	{
	if ( ! op1->IsSingleton(c) )
		return NonReduced(this);

	if ( op2->Tag() == EXPR_LIST )
		return op2->HasReducedOps(c);
	else
		{
		if ( op2->IsSingleton(c) )
			return true;

		return NonReduced(this);
		}
	}

void IndexExpr::ExprDescribe(ODesc* d) const
	{
	op1->Describe(d);
	if ( d->IsReadable() )
		d->Add("[");

	op2->Describe(d);
	if ( d->IsReadable() )
		d->Add("]");
	}

AnyIndexExpr::AnyIndexExpr(IntrusivePtr<Expr> arg_op, int _index)
	: UnaryExpr(EXPR_ANY_INDEX, std::move(arg_op))
	{
	index = _index;
	type = op->Type();
	}

IntrusivePtr<Val> AnyIndexExpr::Fold(Val* v) const
	{
	auto lv = v->AsListVal()->Vals();
	return {NewRef{}, (*lv)[index]};
	}

Expr* AnyIndexExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	return this->Ref();
	}

IntrusivePtr<Expr> AnyIndexExpr::Duplicate()
	{
	return SetSucc(new AnyIndexExpr(op->Duplicate(), index));
	}

void AnyIndexExpr::ExprDescribe(ODesc* d) const
	{
	if ( d->IsReadable() )
		d->Add("(");

	op->Describe(d);

	if ( d->IsReadable() )
		d->Add(")any [");

	d->Add(index);

	if ( d->IsReadable() )
		d->Add("]");
	}


FieldLHSAssignExpr::FieldLHSAssignExpr(IntrusivePtr<Expr> arg_op1,
				IntrusivePtr<Expr> arg_op2,
				const char* _field_name, int _field)
: BinaryExpr(EXPR_FIELD_LHS_ASSIGN, std::move(arg_op1), std::move(arg_op2))
	{
	field_name = _field_name;
	field = _field;
	SetType(op2->Type());

	auto rt = op1->Type()->AsRecordType();
	auto ft = rt->FieldType(field);
	SeatBelts(ft, Type());
	}

IntrusivePtr<Val> FieldLHSAssignExpr::Eval(Frame* f) const
	{
	auto v1 = op1->Eval(f);
	auto v2 = op2->Eval(f);

	if ( v1 && v2 )
		{
		SeatBelts(v2->Type(), Type());

		RecordVal* r = v1->AsRecordVal();
		r->Assign(field, std::move(v2));
		}

	return nullptr;
	}

bool FieldLHSAssignExpr::IsReduced(Reducer* c) const
	{
	ASSERT(op1->IsSingleton(c) && op2->IsReducedFieldAssignment(c));
	return true;
	}

bool FieldLHSAssignExpr::HasReducedOps(Reducer* c) const
	{
	return true;
	}

Expr* FieldLHSAssignExpr::Reduce(Reducer* c,
					IntrusivePtr<Stmt>& red_stmt)
	{
	if ( c->Optimizing() )
		{
		op1 = c->UpdateExpr(op1);
		op2 = c->UpdateExpr(op2);
		}

	return this->Ref();
	}

Expr* FieldLHSAssignExpr::ReduceToSingleton(Reducer* c,
					IntrusivePtr<Stmt>& red_stmt)
	{
	// Yields a statement performing the assignment and for the
	// expression the LHS (but turned into an RHS).
	if ( op1->Tag() != EXPR_NAME )
		Internal("Confusion in FieldLHSAssignExpr::ReduceToSingleton");

	IntrusivePtr<Stmt> op1_red_stmt;
	op1 = {AdoptRef{}, op1->Reduce(c, op1_red_stmt)};

	IntrusivePtr<Expr> assign_expr = Duplicate();
	auto assign_stmt = make_intrusive<ExprStmt>(assign_expr);

	auto field_res = new FieldExpr(op1, field_name);
	IntrusivePtr<Stmt> field_res_stmt;
	auto res = field_res->ReduceToSingleton(c, field_res_stmt);

	red_stmt = MergeStmts(MergeStmts(op1_red_stmt, assign_stmt),
				red_stmt, field_res_stmt);

	return res;
	}

const CompiledStmt FieldLHSAssignExpr::Compile(Compiler* c) const
	{
	auto lhs = Op1()->AsNameExpr();
	auto rhs = Op2();

	if ( rhs->Tag() == EXPR_NAME )
		return c->Field_LHS_AssignFV(lhs, field, Type().get(),
						rhs->AsNameExpr());

	if ( rhs->Tag() == EXPR_CONST )
		return c->Field_LHS_AssignFC(lhs, field, Type().get(),
						rhs->AsConstExpr());

	auto r1 = rhs->GetOp1();
	auto r2 = rhs->GetOp2();

	if ( rhs->Tag() == EXPR_FIELD )
		{
		auto rhs_f = rhs->AsFieldExpr();
		if ( r1->Tag() == EXPR_NAME )
			return c->Field_LHS_AssignFFV(lhs, field, Type().get(),
					r1->AsNameExpr(), rhs_f->Field());
		else
			return c->Field_LHS_AssignFFC(lhs, field, Type().get(),
					r1->AsConstExpr(), rhs_f->Field());
		}

	if ( r1 && r1->IsConst() )
#include "CompilerOpsFieldsDefsC1.h"

	else if ( r2 && r2->IsConst() )
#include "CompilerOpsFieldsDefsC2.h"

	else
#include "CompilerOpsFieldsDefsV.h"
	}

IntrusivePtr<Expr> FieldLHSAssignExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	return SetSucc(new FieldLHSAssignExpr(op1_d, op2_d, field_name, field));
	}

void FieldLHSAssignExpr::ExprDescribe(ODesc* d) const
	{
	op1->Describe(d);
	if ( d->IsReadable() )
		d->Add("$");

	d->Add(field_name);

	if ( d->IsReadable() )
		d->Add(" $= ");

	op2->Describe(d);
	}


FieldExpr::FieldExpr(IntrusivePtr<Expr> arg_op, const char* arg_field_name)
	: UnaryExpr(EXPR_FIELD, std::move(arg_op)),
	  field_name(copy_string(arg_field_name)), td(0), field(0)
	{
	if ( IsError() )
		return;

	if ( ! IsRecord(op->Type()->Tag()) )
		ExprError("not a record");
	else
		{
		RecordType* rt = op->Type()->AsRecordType();
		field = rt->FieldOffset(field_name);

		if ( field < 0 )
			ExprError("no such field in record");
		else
			{
			SetType({NewRef{}, rt->FieldType(field)});
			td = rt->FieldDecl(field);

			if ( rt->IsFieldDeprecated(field) )
				reporter->Warning("%s", rt->GetFieldDeprecationWarning(field, false).c_str());
			}
		}
	}

FieldExpr::~FieldExpr()
	{
	delete [] field_name;
	}

IntrusivePtr<Expr> FieldExpr::MakeLvalue()
	{
	return make_intrusive<RefExpr>(IntrusivePtr{NewRef{}, this});
	}

bool FieldExpr::CanDel() const
	{
	return td->FindAttr(ATTR_DEFAULT) || td->FindAttr(ATTR_OPTIONAL);
	}

void FieldExpr::Assign(Frame* f, IntrusivePtr<Val> v)
	{
	if ( IsError() )
		return;

	if ( auto op_v = op->Eval(f) )
		{
		RecordVal* r = op_v->AsRecordVal();
		r->Assign(field, std::move(v));
		}
	}

void FieldExpr::Delete(Frame* f)
	{
	Assign(f, 0);
	}

IntrusivePtr<Val> FieldExpr::Fold(Val* v) const
	{
	if ( auto result = v->AsRecordVal()->Lookup(field) )
		{
		SeatBelts(result->Type(), Type());
		return result;
		}

	// Check for &default.
	const Attr* def_attr = td ? td->FindAttr(ATTR_DEFAULT) : 0;

	if ( def_attr )
		return def_attr->AttrExpr()->Eval(nullptr);
	else
		{
		RuntimeError("field value missing");
		assert(false);
		return nullptr; // Will never get here, but compiler can't tell.
		}
	}

IntrusivePtr<Expr> FieldExpr::Duplicate()
	{
	return SetSucc(new FieldExpr(op->Duplicate(), field_name));
	}

void FieldExpr::ExprDescribe(ODesc* d) const
	{
	op->Describe(d);
	if ( d->IsReadable() )
		d->Add("$");

	if ( IsError() )
		d->Add("<error>");
	else if ( d->IsReadable() )
		d->Add(field_name);
	else
		d->Add(field);
	}

HasFieldExpr::HasFieldExpr(IntrusivePtr<Expr> arg_op,
                           const char* arg_field_name)
	: UnaryExpr(EXPR_HAS_FIELD, std::move(arg_op)),
	  field_name(arg_field_name), field(0)
	{
	if ( IsError() )
		return;

	if ( ! IsRecord(op->Type()->Tag()) )
		ExprError("not a record");
	else
		{
		RecordType* rt = op->Type()->AsRecordType();
		field = rt->FieldOffset(field_name);

		if ( field < 0 )
			ExprError("no such field in record");
		else if ( rt->IsFieldDeprecated(field) )
			reporter->Warning("%s", rt->GetFieldDeprecationWarning(field, true).c_str());

		SetType(base_type(TYPE_BOOL));
		}
	}

HasFieldExpr::~HasFieldExpr()
	{
	delete field_name;
	}

IntrusivePtr<Val> HasFieldExpr::Fold(Val* v) const
	{
	auto rv = v->AsRecordVal();
	return {AdoptRef{}, val_mgr->GetBool(rv->Lookup(field) != nullptr)};
	}

IntrusivePtr<Expr> HasFieldExpr::Duplicate()
	{
	return SetSucc(new HasFieldExpr(op->Duplicate(), field_name));
	}

void HasFieldExpr::ExprDescribe(ODesc* d) const
	{
	op->Describe(d);

	if ( d->IsReadable() )
		d->Add("?$");

	if ( IsError() )
		d->Add("<error>");
	else if ( d->IsReadable() )
		d->Add(field_name);
	else
		d->Add(field);
	}

RecordConstructorExpr::RecordConstructorExpr(IntrusivePtr<ListExpr> constructor_list)
	: UnaryExpr(EXPR_RECORD_CONSTRUCTOR, std::move(constructor_list))
	{
	if ( IsError() )
		return;

	// Spin through the list, which should be comprised only of
	// record-field-assign expressions, and build up a
	// record type to associate with this constructor.
	const expr_list& exprs = op->AsListExpr()->Exprs();
	type_decl_list* record_types = new type_decl_list(exprs.length());

	for ( const auto& e : exprs )
		{
		if ( e->Tag() != EXPR_FIELD_ASSIGN )
			{
			Error("bad type in record constructor", e);
			SetError();
			continue;
			}

		FieldAssignExpr* field = e->AsFieldAssignExpr();
		IntrusivePtr<BroType> field_type = field->Type();
		char* field_name = copy_string(field->FieldName());
		record_types->push_back(new TypeDecl(std::move(field_type), field_name));
		}

	SetType(make_intrusive<RecordType>(record_types));
	}

RecordConstructorExpr::RecordConstructorExpr(IntrusivePtr<RecordType> known_rt,
				IntrusivePtr<ListExpr> constructor_list)
	: UnaryExpr(EXPR_RECORD_CONSTRUCTOR, std::move(constructor_list))
	{
	if ( IsError() )
		return;

	SetType(known_rt);
	rt = known_rt;

	const expr_list& exprs = op->AsListExpr()->Exprs();
	map = new int[exprs.length()];

	int i = 0;
	for ( const auto& e : exprs )
		{
		if ( e->Tag() != EXPR_FIELD_ASSIGN )
			{
			Error("bad type in record constructor", e);
			SetError();
			continue;
			}

		auto field = e->AsFieldAssignExpr();
		int index = known_rt->FieldOffset(field->FieldName());

		if ( index < 0 )
			{
			Error("no such field in record", e);
			SetError();
			continue;
			}

		auto known_ft = known_rt->FieldType(index);

		if ( ! field->PromoteTo(known_ft) )
			SetError();

		map[i++] = index;
		}
	}

IntrusivePtr<Val> RecordConstructorExpr::InitVal(const BroType* t, IntrusivePtr<Val> aggr) const
	{
	auto v = Eval(nullptr);

	if ( v )
		{
		RecordVal* rv = v->AsRecordVal();
		auto ar = rv->CoerceTo(t->AsRecordType(), aggr.release());

		if ( ar )
			return ar;
		}

	Error("bad record initializer");
	return nullptr;
	}

IntrusivePtr<Val> RecordConstructorExpr::Fold(Val* v) const
	{
	ListVal* lv = v->AsListVal();
	RecordType* rt = type->AsRecordType();

	if ( ! map && lv->Length() != rt->NumFields() )
		RuntimeErrorWithCallStack("inconsistency evaluating record constructor");

	auto rv = make_intrusive<RecordVal>(rt);

	for ( int i = 0; i < lv->Length(); ++i )
		{
		int ind = map ? map[i] : i;
		rv->Assign(ind, lv->Index(i)->Ref());
		}

	return rv;
	}

bool RecordConstructorExpr::HasReducedOps(Reducer* c) const
	{
	expr_list& exprs = op->AsListExpr()->Exprs();

	loop_over_list(exprs, i)
		{
		auto e_i = exprs[i];
		if ( ! e_i->AsFieldAssignExpr()->Op()->IsSingleton(c) )
			return false;
		}

	return true;
	}

Expr* RecordConstructorExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	red_stmt = ReduceToSingletons(c);

	if ( c->Optimizing() )
		return this->Ref();
	else
		return AssignToTemporary(c, red_stmt);
	}

IntrusivePtr<Stmt> RecordConstructorExpr::ReduceToSingletons(Reducer* c)
	{
	IntrusivePtr<Stmt> red_stmt;
	expr_list& exprs = op->AsListExpr()->Exprs();

	// Could consider merging this code with that for ListExpr::Reduce.
	loop_over_list(exprs, i)
		{
		auto e_i = exprs[i];
		auto fa_i = e_i->AsFieldAssignExpr();
		auto fa_i_rhs = e_i->GetOp1();

		if ( c->Optimizing() )
			{
			fa_i->SetOp1(c->UpdateExpr(fa_i_rhs));
			continue;
			}

		if ( fa_i_rhs->IsSingleton(c) )
			continue;

		IntrusivePtr<Stmt> e_stmt;
		IntrusivePtr<Expr> rhs_red =
			{AdoptRef{}, fa_i_rhs->ReduceToSingleton(c, e_stmt)};
		fa_i->SetOp1(rhs_red);

		if ( e_stmt )
			red_stmt = MergeStmts(red_stmt, e_stmt);
		}

	return red_stmt;
	}

IntrusivePtr<Expr> RecordConstructorExpr::Duplicate()
	{
	auto op_l = op->Duplicate()->AsListExprPtr();

	if ( map )
		return SetSucc(new RecordConstructorExpr(rt, op_l));
	else
		return SetSucc(new RecordConstructorExpr(op_l));
	}

void RecordConstructorExpr::ExprDescribe(ODesc* d) const
	{
	if ( map )
		{
		d->Add(rt->GetName());
		d->Add("(");
		op->Describe(d);
		d->Add(")");
		}
	else
		{
		d->Add("[");
		op->Describe(d);
		d->Add("]");
		}
	}

TableConstructorExpr::TableConstructorExpr(IntrusivePtr<ListExpr> constructor_list,
                                           attr_list* arg_attrs,
                                           IntrusivePtr<BroType> arg_type)
	: UnaryExpr(EXPR_TABLE_CONSTRUCTOR, std::move(constructor_list)),
	  attrs(nullptr)
	{
	if ( IsError() )
		return;

	if ( arg_type )
		{
		if ( ! arg_type->IsTable() )
			{
			Error("bad table constructor type", arg_type.get());
			SetError();
			return;
			}

		SetType(std::move(arg_type));
		}
	else
		{
		if ( op->AsListExpr()->Exprs().empty() )
			SetType(make_intrusive<TableType>(make_intrusive<TypeList>(base_type(TYPE_ANY)), nullptr));
		else
			{
			SetType(init_type(op.get()));

			if ( ! type )
				SetError();

			else if ( type->Tag() != TYPE_TABLE ||
				  type->AsTableType()->IsSet() )
				SetError("values in table(...) constructor do not specify a table");
			}
		}

	attrs = arg_attrs ? new Attributes(arg_attrs, type, false, false) : 0;

	type_list* indices = type->AsTableType()->Indices()->Types();
	const expr_list& cle = op->AsListExpr()->Exprs();

	// check and promote all index expressions in ctor list
	for ( const auto& expr : cle )
		{
		if ( expr->Tag() != EXPR_ASSIGN )
			continue;

		Expr* idx_expr = expr->AsAssignExpr()->Op1();

		if ( idx_expr->Tag() != EXPR_LIST )
			continue;

		expr_list& idx_exprs = idx_expr->AsListExpr()->Exprs();

		if ( idx_exprs.length() != indices->length() )
			continue;

		loop_over_list(idx_exprs, j)
			{
			Expr* idx = idx_exprs[j];

			auto promoted_idx = check_and_promote_expr(idx, (*indices)[j]);

			if ( promoted_idx )
				{
				if ( promoted_idx.get() != idx )
					{
					Unref(idx);
					idx_exprs.replace(j, promoted_idx.release());
					}

				continue;
				}

			ExprError("inconsistent types in table constructor");
			}
		}
	}

IntrusivePtr<Val> TableConstructorExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return nullptr;

	auto aggr = make_intrusive<TableVal>(IntrusivePtr{NewRef{}, Type()->AsTableType()},
	                                     IntrusivePtr{NewRef{}, attrs});
	const expr_list& exprs = op->AsListExpr()->Exprs();

	for ( const auto& expr : exprs )
		expr->EvalIntoAggregate(type.get(), aggr.get(), f);

	aggr->InitDefaultFunc(f);

	return aggr;
	}

bool TableConstructorExpr::HasReducedOps(Reducer* c) const
	{
	const expr_list& exprs = op->AsListExpr()->Exprs();

	for ( const auto& expr : exprs )
		{
		auto a = expr->AsAssignExpr();
		// LHS is a list, not a singleton.
		if ( ! a->GetOp1()->HasReducedOps(c) ||
		     ! a->GetOp2()->IsSingleton(c) )
			return NonReduced(this);
		}

	return true;
	}

Expr* TableConstructorExpr::Reduce(Reducer* c,
					IntrusivePtr<Stmt>& red_stmt)
	{
	red_stmt = ReduceToSingletons(c);

	if ( c->Optimizing() )
		return this->Ref();
	else
		return AssignToTemporary(c, red_stmt);
	}

IntrusivePtr<Stmt> TableConstructorExpr::ReduceToSingletons(Reducer* c)
	{
	// Need to process the list of initializers directly, as
	// they may be expressed as AssignExpr's, and those get
	// treated quite differently during reduction.
	const expr_list& exprs = op->AsListExpr()->Exprs();

	IntrusivePtr<Stmt> red_stmt;

	for ( const auto& expr : exprs )
		{
		if ( expr->Tag() == EXPR_ASSIGN )
			{
			auto a = expr->AsAssignExpr();
			auto op1 = a->GetOp1();
			auto op2 = a->GetOp2();

			if ( c->Optimizing() )
				{
				a->SetOp1(c->UpdateExpr(op1));
				a->SetOp2(c->UpdateExpr(op2));
				continue;
				}

			IntrusivePtr<Stmt> red1_stmt;
			IntrusivePtr<Stmt> red2_stmt;

			a->SetOp1({AdoptRef{},
					op1->ReduceToSingleton(c, red1_stmt)});
			a->SetOp2({AdoptRef{},
					op2->ReduceToSingleton(c, red2_stmt)});

			red_stmt = MergeStmts(red_stmt, red1_stmt, red2_stmt);
			}

		else
			reporter->InternalError("confused in TableConstructorExpr::Reduce");
		}

	return red_stmt;
	}

IntrusivePtr<Expr> TableConstructorExpr::Duplicate()
	{
	auto op_l = op->Duplicate()->AsListExprPtr();
	auto a = attrs ? attrs->Attrs() : nullptr;

	if ( op->AsListExpr()->Exprs().empty() )
		return SetSucc(new TableConstructorExpr(op_l, a, nullptr));
	else
		return SetSucc(new TableConstructorExpr(op_l, a, type));
	}

IntrusivePtr<Val> TableConstructorExpr::InitVal(const BroType* t, IntrusivePtr<Val> aggr) const
	{
	if ( IsError() )
		return nullptr;

	TableType* tt = Type()->AsTableType();
	auto tval = aggr ?
	        IntrusivePtr<TableVal>{AdoptRef{}, aggr.release()->AsTableVal()} :
	        make_intrusive<TableVal>(IntrusivePtr{NewRef{}, tt}, IntrusivePtr{NewRef{}, attrs});
	const expr_list& exprs = op->AsListExpr()->Exprs();

	for ( const auto& expr : exprs )
		expr->EvalIntoAggregate(t, tval.get(), 0);

	return tval;
	}

void TableConstructorExpr::ExprDescribe(ODesc* d) const
	{
	d->Add("table(");
	op->Describe(d);
	d->Add(")");
	}

SetConstructorExpr::SetConstructorExpr(IntrusivePtr<ListExpr> constructor_list,
                                       attr_list* arg_attrs,
                                       IntrusivePtr<BroType> arg_type)
	: UnaryExpr(EXPR_SET_CONSTRUCTOR, std::move(constructor_list)),
	  attrs(nullptr)
	{
	if ( IsError() )
		return;

	if ( arg_type )
		{
		if ( ! arg_type->IsSet() )
			{
			Error("bad set constructor type", arg_type.get());
			SetError();
			return;
			}

		SetType(std::move(arg_type));
		}
	else
		{
		if ( op->AsListExpr()->Exprs().empty() )
			SetType(make_intrusive<::SetType>(make_intrusive<TypeList>(base_type(TYPE_ANY)), nullptr));
		else
			SetType(init_type(op.get()));
		}

	if ( ! type )
		SetError();

	else if ( type->Tag() != TYPE_TABLE || ! type->AsTableType()->IsSet() )
		SetError("values in set(...) constructor do not specify a set");

	attrs = arg_attrs ? new Attributes(arg_attrs, type, false, false) : 0;

	type_list* indices = type->AsTableType()->Indices()->Types();
	expr_list& cle = op->AsListExpr()->Exprs();

	if ( indices->length() == 1 )
		{
		if ( ! check_and_promote_exprs_to_type(op->AsListExpr(),
		                                       (*indices)[0]) )
			ExprError("inconsistent type in set constructor");
		}

	else if ( indices->length() > 1 )
		{
		// Check/promote each expression in composite index.
		loop_over_list(cle, i)
			{
			Expr* ce = cle[i];
			ListExpr* le = ce->AsListExpr();

			if ( ce->Tag() == EXPR_LIST &&
			     check_and_promote_exprs(le, type->AsTableType()->Indices()) )
				{
				if ( le != cle[i] )
					cle.replace(i, le);

				continue;
				}

			ExprError("inconsistent types in set constructor");
			}
		}
	}

IntrusivePtr<Val> SetConstructorExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return nullptr;

	auto aggr = make_intrusive<TableVal>(IntrusivePtr{NewRef{}, type->AsTableType()},
	                                     IntrusivePtr{NewRef{}, attrs});
	const expr_list& exprs = op->AsListExpr()->Exprs();

	for ( const auto& expr : exprs )
		{
		auto element = expr->Eval(f);
		aggr->Assign(element.get(), 0);
		}

	return aggr;
	}

bool SetConstructorExpr::HasReducedOps(Reducer* c) const
	{
	return op->IsReduced(c);
	}

Expr* SetConstructorExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	// We rely on the fact that ListExpr's don't change into
	// temporaries.
	red_stmt = nullptr;

	Unref(op->Reduce(c, red_stmt));

	if ( c->Optimizing() )
		return this->Ref();
	else
		return AssignToTemporary(c, red_stmt);
	}

IntrusivePtr<Expr> SetConstructorExpr::Duplicate()
	{
	auto op_l = op->Duplicate()->AsListExprPtr();
	auto a = attrs ? attrs->Attrs() : nullptr;

	if ( op->AsListExpr()->Exprs().empty() )
		return SetSucc(new SetConstructorExpr(op_l, a, nullptr));
	else
		return SetSucc(new SetConstructorExpr(op_l, a, type));
	}

IntrusivePtr<Stmt> SetConstructorExpr::ReduceToSingletons(Reducer* c)
	{
	return op->ReduceToSingletons(c);
	}

IntrusivePtr<Val> SetConstructorExpr::InitVal(const BroType* t, IntrusivePtr<Val> aggr) const
	{
	if ( IsError() )
		return nullptr;

	const BroType* index_type = t->AsTableType()->Indices();
	TableType* tt = Type()->AsTableType();
	auto tval = aggr ?
	        IntrusivePtr<TableVal>{AdoptRef{}, aggr.release()->AsTableVal()} :
	        make_intrusive<TableVal>(IntrusivePtr{NewRef{}, tt}, IntrusivePtr{NewRef{}, attrs});
	const expr_list& exprs = op->AsListExpr()->Exprs();

	for ( const auto& e : exprs )
		{
		auto element = check_and_promote(e->Eval(nullptr), index_type, true);

		if ( ! element || ! tval->Assign(element.get(), 0) )
			{
			Error(fmt("initialization type mismatch in set"), e);
			return nullptr;
			}
		}

	return tval;
	}

void SetConstructorExpr::ExprDescribe(ODesc* d) const
	{
	d->Add("set(");
	op->Describe(d);
	d->Add(")");
	}

VectorConstructorExpr::VectorConstructorExpr(IntrusivePtr<ListExpr> constructor_list,
                                             IntrusivePtr<BroType> arg_type)
	: UnaryExpr(EXPR_VECTOR_CONSTRUCTOR, std::move(constructor_list))
	{
	if ( IsError() )
		return;

	if ( arg_type )
		{
		if ( arg_type->Tag() != TYPE_VECTOR )
			{
			Error("bad vector constructor type", arg_type.get());
			SetError();
			return;
			}

		SetType(std::move(arg_type));
		}
	else
		{
		if ( op->AsListExpr()->Exprs().empty() )
			{
			// vector().
			// By default, assign VOID type here. A vector with
			// void type set is seen as an unspecified vector.
			SetType(make_intrusive<::VectorType>(base_type(TYPE_VOID)));
			return;
			}

		if ( auto t = merge_type_list(op->AsListExpr()) )
			SetType(make_intrusive<VectorType>(std::move(t)));
		else
			{
			SetError();
			return;
			}
		}

	if ( ! check_and_promote_exprs_to_type(op->AsListExpr(),
					       type->AsVectorType()->YieldType()) )
		ExprError("inconsistent types in vector constructor");
	}

IntrusivePtr<Val> VectorConstructorExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return nullptr;

	auto vec = make_intrusive<VectorVal>(Type()->AsVectorType());
	const expr_list& exprs = op->AsListExpr()->Exprs();

	loop_over_list(exprs, i)
		{
		Expr* e = exprs[i];

		if ( ! vec->Assign(i, e->Eval(f)) )
			{
			RuntimeError(fmt("type mismatch at index %d", i));
			return nullptr;
			}
		}

	return vec;
	}

bool VectorConstructorExpr::HasReducedOps(Reducer* c) const
	{
	return Op()->HasReducedOps(c);
	}

IntrusivePtr<Val> VectorConstructorExpr::InitVal(const BroType* t, IntrusivePtr<Val> aggr) const
	{
	if ( IsError() )
		return nullptr;

	VectorType* vt = Type()->AsVectorType();
	auto vec = aggr ?
	        IntrusivePtr<VectorVal>{AdoptRef{}, aggr.release()->AsVectorVal()} :
	        make_intrusive<VectorVal>(vt);
	const expr_list& exprs = op->AsListExpr()->Exprs();

	loop_over_list(exprs, i)
		{
		Expr* e = exprs[i];
		auto v = check_and_promote(e->Eval(nullptr), t->YieldType(), true);

		if ( ! v || ! vec->Assign(i, std::move(v)) )
			{
			Error(fmt("initialization type mismatch at index %d", i), e);
			return nullptr;
			}
		}

	return vec;
	}

IntrusivePtr<Expr> VectorConstructorExpr::Duplicate()
	{
	auto op_l = op->Duplicate()->AsListExprPtr();

	if ( op->AsListExpr()->Exprs().empty() )
		return SetSucc(new VectorConstructorExpr(op_l, nullptr));
	else
		return SetSucc(new VectorConstructorExpr(op_l, type));
	}

void VectorConstructorExpr::ExprDescribe(ODesc* d) const
	{
	d->Add("vector(");
	op->Describe(d);
	d->Add(")");
	}

FieldAssignExpr::FieldAssignExpr(const char* arg_field_name,
                                 IntrusivePtr<Expr> value)
	: UnaryExpr(EXPR_FIELD_ASSIGN, std::move(value)), field_name(arg_field_name)
	{
	SetType(op->Type());
	}

bool FieldAssignExpr::PromoteTo(BroType* t)
	{
	op = check_and_promote_expr(op.get(), t);
	return op != nullptr;
	}

void FieldAssignExpr::EvalIntoAggregate(const BroType* t, Val* aggr, Frame* f)
	const
	{
	if ( IsError() )
		return;

	if ( auto v = op->Eval(f) )
		{
		RecordVal* rec = aggr->AsRecordVal();
		const RecordType* rt = t->AsRecordType();

		int idx = rt->FieldOffset(field_name.c_str());

		if ( idx < 0 )
			reporter->InternalError("Missing record field: %s",
			                        field_name.c_str());

		rec->Assign(idx, std::move(v));
		}
	}

Expr* FieldAssignExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( c->Optimizing() )
		{
		op = c->UpdateExpr(op);
		return this->Ref();
		}

	red_stmt = nullptr;

	if ( ! op->IsReduced(c) )
		op = {AdoptRef{}, op->ReduceToSingleton(c, red_stmt)};

	// Doesn't seem worth checking for constant folding.

	return AssignToTemporary(c, red_stmt);
	}

IntrusivePtr<Expr> FieldAssignExpr::Duplicate()
	{
	auto op_dup = op->Duplicate();
	return SetSucc(new FieldAssignExpr(field_name.c_str(), op_dup));
	}

bool FieldAssignExpr::IsRecordElement(TypeDecl* td) const
	{
	if ( td )
		{
		td->type = op->Type();
		td->id = copy_string(field_name.c_str());
		}

	return true;
	}

void FieldAssignExpr::ExprDescribe(ODesc* d) const
	{
	d->Add("$");
	d->Add(FieldName());
	d->Add("=");

	if ( op )
		op->Describe(d);
	else
		d->Add("<error>");
	}

ArithCoerceExpr::ArithCoerceExpr(IntrusivePtr<Expr> arg_op, TypeTag t)
: UnaryExpr(EXPR_ARITH_COERCE, std::move(arg_op))
	{
	if ( IsError() )
		return;

	TypeTag bt = op->Type()->Tag();
	TypeTag vbt = bt;

	if ( IsVector(bt) )
		{
		SetType(make_intrusive<VectorType>(base_type(t)));
		vbt = op->Type()->AsVectorType()->YieldType()->Tag();
		}
	else
		SetType(base_type(t));

	if ( (bt == TYPE_ENUM) != (t == TYPE_ENUM) )
		ExprError("can't convert to/from enumerated type");

	else if ( ! IsArithmetic(t) && ! IsBool(t) &&
		  t != TYPE_TIME && t != TYPE_INTERVAL )
		ExprError("bad coercion");

	else if ( ! IsArithmetic(bt) && ! IsBool(bt) &&
		  ! IsArithmetic(vbt) && ! IsBool(vbt) )
		ExprError("bad coercion value");
	}

IntrusivePtr<Val> ArithCoerceExpr::FoldSingleVal(IntrusivePtr<Val> v) const
	{
	return check_and_promote(v, type.get(), false, location);
	}

IntrusivePtr<Val> ArithCoerceExpr::Fold(Val* v) const
	{
	InternalTypeTag t = type->InternalType();

	if ( ! is_vector(v) )
		{
		// Our result type might be vector, in which case this
		// invocation is being done per-element rather than on
		// the whole vector.  Correct the type tag if necessary.
		if ( type->Tag() == TYPE_VECTOR )
			t = Type()->AsVectorType()->YieldType()->InternalType();

		return FoldSingleVal({NewRef{}, v});
		}

	t = Type()->AsVectorType()->YieldType()->InternalType();

	VectorVal* vv = v->AsVectorVal();
	auto result = make_intrusive<VectorVal>(Type()->AsVectorType());

	for ( unsigned int i = 0; i < vv->Size(); ++i )
		{
		if ( auto elt = vv->Lookup(i) )
			result->Assign(i, FoldSingleVal(elt));
		else
			result->Assign(i, 0);
		}

	return result;
	}

bool ArithCoerceExpr::WillTransform(Reducer* c) const
	{
	return op->Tag() == EXPR_CONST &&
		IsArithmetic(op->AsConstExpr()->Value()->Type()->Tag());
	}

Expr* ArithCoerceExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( c->Optimizing() )
		op = c->UpdateExpr(op);

	red_stmt = nullptr;

	auto t = type->InternalType();

	if ( ! op->IsReduced(c) )
		op = {AdoptRef{}, op->ReduceToSingleton(c, red_stmt)};

	if ( op->Tag() == EXPR_CONST )
		{
		auto c = op->AsConstExpr()->ValuePtr();

		if ( IsArithmetic(c->Type()->Tag()) )
			return new ConstExpr(FoldSingleVal(c));
		}

	if ( c->Optimizing() )
		return this->Ref();

	auto bt = op->Type()->InternalType();

	if ( t == bt )
		return op->Ref();

	return AssignToTemporary(c, red_stmt);
	}

IntrusivePtr<Expr> ArithCoerceExpr::Duplicate()
	{
	auto op_dup = op->Duplicate();

	TypeTag tag;

	if ( type->Tag() == TYPE_VECTOR )
		tag = type->AsVectorType()->YieldType()->Tag();
	else
		tag = type->Tag();

	return SetSucc(new ArithCoerceExpr(op_dup, tag));
	}

RecordCoerceExpr::RecordCoerceExpr(IntrusivePtr<Expr> arg_op,
                                   IntrusivePtr<RecordType> r)
	: UnaryExpr(EXPR_RECORD_COERCE, std::move(arg_op)),
	  map(nullptr), map_size(0)
	{
	if ( IsError() )
		return;

	SetType(std::move(r));

	if ( Type()->Tag() != TYPE_RECORD )
		ExprError("coercion to non-record");

	else if ( op->Type()->Tag() != TYPE_RECORD )
		ExprError("coercion of non-record to record");

	else
		{
		RecordType* t_r = type->AsRecordType();
		RecordType* sub_r = op->Type()->AsRecordType();

		map_size = t_r->NumFields();
		map = new int[map_size];

		int i;
		for ( i = 0; i < map_size; ++i )
			map[i] = -1;	// -1 = field is not mapped

		for ( i = 0; i < sub_r->NumFields(); ++i )
			{
			int t_i = t_r->FieldOffset(sub_r->FieldName(i));
			if ( t_i < 0 )
				{
				ExprError(fmt("orphaned field \"%s\" in record coercion",
				              sub_r->FieldName(i)));
				break;
				}

			BroType* sub_t_i = sub_r->FieldType(i);
			BroType* sup_t_i = t_r->FieldType(t_i);

			if ( ! same_type(sup_t_i, sub_t_i) )
				{
				auto is_arithmetic_promotable = [](BroType* sup, BroType* sub) -> bool
					{
					auto sup_tag = sup->Tag();
					auto sub_tag = sub->Tag();

					if ( ! BothArithmetic(sup_tag, sub_tag) )
						return false;

					if ( sub_tag == TYPE_DOUBLE && IsIntegral(sup_tag) )
						return false;

					if ( sub_tag == TYPE_INT && sup_tag == TYPE_COUNT )
						return false;

					return true;
					};

				auto is_record_promotable = [](BroType* sup, BroType* sub) -> bool
					{
					if ( sup->Tag() != TYPE_RECORD )
						return false;

					if ( sub->Tag() != TYPE_RECORD )
						return false;

					return record_promotion_compatible(sup->AsRecordType(),
					                                   sub->AsRecordType());
					};

				if ( ! is_arithmetic_promotable(sup_t_i, sub_t_i) &&
				     ! is_record_promotable(sup_t_i, sub_t_i) )
					{
					string error_msg = fmt(
						"type clash for field \"%s\"", sub_r->FieldName(i));
					Error(error_msg.c_str(), sub_t_i);
					SetError();
					break;
					}
				}

			map[t_i] = i;
			}

		if ( IsError() )
			return;

		for ( i = 0; i < map_size; ++i )
			{
			if ( map[i] == -1 )
				{
				if ( ! t_r->FieldDecl(i)->FindAttr(ATTR_OPTIONAL) )
					{
					string error_msg = fmt(
						"non-optional field \"%s\" missing", t_r->FieldName(i));
					Error(error_msg.c_str());
					SetError();
					break;
					}
				}
			else if ( t_r->IsFieldDeprecated(i) )
				reporter->Warning("%s", t_r->GetFieldDeprecationWarning(i, false).c_str());
			}
		}
	}

RecordCoerceExpr::~RecordCoerceExpr()
	{
	delete [] map;
	}

IntrusivePtr<Val> RecordCoerceExpr::InitVal(const BroType* t, IntrusivePtr<Val> aggr) const
	{
	if ( auto v = Eval(nullptr) )
		{
		RecordVal* rv = v->AsRecordVal();
		if ( auto ar = rv->CoerceTo(t->AsRecordType(), aggr.release()) )
			return ar;
		}

	Error("bad record initializer");
	return nullptr;
	}

IntrusivePtr<Val> RecordCoerceExpr::Fold(Val* v) const
	{
	auto rt = Type()->AsRecordType();
	return coerce_to_record(rt, v, map, map_size);
	}

IntrusivePtr<Expr> RecordCoerceExpr::Duplicate()
	{
	auto op_dup = op->Duplicate();
	auto rt = Type()->AsRecordType();
	IntrusivePtr<RecordType> rt_p = {NewRef{}, rt};
	return SetSucc(new RecordCoerceExpr(op_dup, rt_p));
	}

extern IntrusivePtr<RecordVal> coerce_to_record(RecordType* rt, Val* v,
						int* map, int map_size)
	{
	auto val = make_intrusive<RecordVal>(rt);
	RecordType* val_type = val->Type()->AsRecordType();

	RecordVal* rv = v->AsRecordVal();

	for ( int i = 0; i < map_size; ++i )
		{
		if ( map[i] >= 0 )
			{
			IntrusivePtr<Val> rhs = rv->Lookup(map[i]);

			if ( ! rhs )
				{
				auto rv_rt = rv->Type()->AsRecordType();
				const Attr* def = rv_rt->FieldDecl(map[i])->
							FindAttr(ATTR_DEFAULT);

				if ( def )
					rhs = def->AttrExpr()->Eval(nullptr);
				}

			assert(rhs || rt->FieldDecl(i)->FindAttr(ATTR_OPTIONAL));

			if ( ! rhs )
				{
				// Optional field is missing.
				val->Assign(i, nullptr);
				continue;
				}

			BroType* rhs_type = rhs->Type();
			BroType* field_type = val_type->FieldType(i);

			if ( rhs_type->Tag() == TYPE_RECORD &&
			     field_type->Tag() == TYPE_RECORD &&
			     ! same_type(rhs_type, field_type) )
				{
				if ( auto new_val = rhs->AsRecordVal()->CoerceTo(field_type->AsRecordType()) )
					rhs = std::move(new_val);
				}
			else if ( BothArithmetic(rhs_type->Tag(), field_type->Tag()) &&
			          ! same_type(rhs_type, field_type) )
				{
				auto new_val = check_and_promote(rhs, field_type, false);
				rhs = std::move(new_val);
				}

			val->Assign(i, std::move(rhs));
			}
		else
			{
			if ( const Attr* def = rt->FieldDecl(i)->FindAttr(ATTR_DEFAULT) )
				{
				auto def_val = def->AttrExpr()->Eval(nullptr);
				BroType* def_type = def_val->Type();
				BroType* field_type = rt->FieldType(i);

				if ( def_type->Tag() == TYPE_RECORD &&
				     field_type->Tag() == TYPE_RECORD &&
				     ! same_type(def_type, field_type) )
					{
					auto tmp = def_val->AsRecordVal()->CoerceTo(
					        field_type->AsRecordType());

					if ( tmp )
						def_val = std::move(tmp);
					}

				val->Assign(i, std::move(def_val));
				}
			else
				val->Assign(i, nullptr);
			}
		}

	return val;
	}

TableCoerceExpr::TableCoerceExpr(IntrusivePtr<Expr> arg_op,
                                 IntrusivePtr<TableType> r)
	: UnaryExpr(EXPR_TABLE_COERCE, std::move(arg_op))
	{
	if ( IsError() )
		return;

	SetType(std::move(r));

	if ( Type()->Tag() != TYPE_TABLE )
		ExprError("coercion to non-table");

	else if ( op->Type()->Tag() != TYPE_TABLE )
		ExprError("coercion of non-table/set to table/set");
	}


IntrusivePtr<Val> TableCoerceExpr::Fold(Val* v) const
	{
	TableVal* tv = v->AsTableVal();

	if ( tv->Size() > 0 )
		RuntimeErrorWithCallStack("coercion of non-empty table/set");

	return make_intrusive<TableVal>(IntrusivePtr{NewRef{}, Type()->AsTableType()},
	                                IntrusivePtr{NewRef{}, tv->Attrs()});
	}

IntrusivePtr<Expr> TableCoerceExpr::Duplicate()
	{
	auto op_dup = op->Duplicate();
	auto tt = Type()->AsTableType();
	IntrusivePtr<TableType> tt_p = {NewRef{}, tt};
	return SetSucc(new TableCoerceExpr(op_dup, tt_p));
	}

VectorCoerceExpr::VectorCoerceExpr(IntrusivePtr<Expr> arg_op,
                                   IntrusivePtr<VectorType> v)
	: UnaryExpr(EXPR_VECTOR_COERCE, std::move(arg_op))
	{
	if ( IsError() )
		return;

	SetType(std::move(v));

	if ( Type()->Tag() != TYPE_VECTOR )
		ExprError("coercion to non-vector");

	else if ( op->Type()->Tag() != TYPE_VECTOR )
		ExprError("coercion of non-vector to vector");
	}

IntrusivePtr<Val> VectorCoerceExpr::Fold(Val* v) const
	{
	VectorVal* vv = v->AsVectorVal();

	if ( vv->Size() > 0 )
		RuntimeErrorWithCallStack("coercion of non-empty vector");

	return make_intrusive<VectorVal>(Type()->Ref()->AsVectorType());
	}

IntrusivePtr<Expr> VectorCoerceExpr::Duplicate()
	{
	auto op_dup = op->Duplicate();
	auto vt = Type()->AsVectorType();
	IntrusivePtr<VectorType> vt_p = {NewRef{}, vt};
	return SetSucc(new VectorCoerceExpr(op_dup, vt_p));
	}


CoerceToAnyExpr::CoerceToAnyExpr(IntrusivePtr<Expr> arg_op)
	: UnaryExpr(EXPR_TO_ANY_COERCE, std::move(arg_op))
	{
	type = base_type(TYPE_ANY);
	}

IntrusivePtr<Val> CoerceToAnyExpr::Fold(Val* v) const
	{
	return {NewRef{}, v};
	}

IntrusivePtr<Expr> CoerceToAnyExpr::Duplicate()
	{
	return SetSucc(new CoerceToAnyExpr(op->Duplicate()));
	}


CoerceFromAnyExpr::CoerceFromAnyExpr(IntrusivePtr<Expr> arg_op,
					IntrusivePtr<BroType> to_type)
	: UnaryExpr(EXPR_FROM_ANY_COERCE, std::move(arg_op))
	{
	type = to_type;
	}

IntrusivePtr<Val> CoerceFromAnyExpr::Fold(Val* v) const
	{
	auto t = Type()->Tag();
	auto vt = v->Type()->Tag();

	if ( vt != t && vt != TYPE_ERROR )
		RuntimeError("incompatible \"any\" type");

	return {NewRef{}, v};
	}

IntrusivePtr<Expr> CoerceFromAnyExpr::Duplicate()
	{
	return SetSucc(new CoerceFromAnyExpr(op->Duplicate(), type));
	}


ScheduleTimer::ScheduleTimer(const EventHandlerPtr& arg_event, zeek::Args arg_args,
                             double t)
	: Timer(t, TIMER_SCHEDULE),
	  event(arg_event), args(std::move(arg_args))
	{
	}

ScheduleTimer::~ScheduleTimer()
	{
	}

void ScheduleTimer::Dispatch(double /* t */, bool /* is_expire */)
	{
	if ( event )
		mgr.Enqueue(event, std::move(args));
	}

ScheduleExpr::ScheduleExpr(IntrusivePtr<Expr> arg_when,
                           IntrusivePtr<EventExpr> arg_event)
	: Expr(EXPR_SCHEDULE),
	  when(std::move(arg_when)), event(std::move(arg_event))
	{
	if ( IsError() || when->IsError() || event->IsError() )
		return;

	TypeTag bt = when->Type()->Tag();

	if ( bt != TYPE_TIME && bt != TYPE_INTERVAL )
		ExprError("schedule expression requires a time or time interval");
	else
		SetType(base_type(TYPE_TIMER));
	}

bool ScheduleExpr::IsPure() const
	{
	return false;
	}

bool ScheduleExpr::IsReduced(Reducer* c) const
	{
	return when->IsReduced(c) && event->IsReduced(c);
	}

bool ScheduleExpr::HasReducedOps(Reducer* c) const
	{
	if ( when->IsSingleton(c) && event->IsSingleton(c) )
		return true;

	return NonReduced(this);
	}

Expr* ScheduleExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( c->Optimizing() )
		{
		when = c->UpdateExpr(when);
		auto e = c->UpdateExpr(event);
		auto ev = e->AsEventExpr();
		event = {NewRef{}, ev};
		}

	red_stmt = nullptr;

	if ( ! when->IsReduced(c) )
		when = {AdoptRef{}, when->Reduce(c, red_stmt)};

	IntrusivePtr<Stmt> red2_stmt;
	// We assume that EventExpr won't transform itself fundamentally.
	Unref(event->Reduce(c, red2_stmt));

	red_stmt = MergeStmts(red_stmt, red2_stmt);

	return this->Ref();
	}

Expr* ScheduleExpr::Inline(Inliner* inl)
	{
	when = {NewRef{}, when->Inline(inl)};
	auto e = event->Inline(inl);
	auto ev = e->AsEventExpr();
	event = {NewRef{}, ev};

	return this->Ref();
	}

IntrusivePtr<Val> ScheduleExpr::Eval(Frame* f) const
	{
	if ( terminating )
		return nullptr;

	auto when_val = when->Eval(f);

	if ( ! when_val )
		return nullptr;

	double dt = when_val->InternalDouble();

	if ( when->Type()->Tag() == TYPE_INTERVAL )
		dt += network_time;

	auto args = eval_list(f, event->Args());

	if ( args )
		timer_mgr->Add(new ScheduleTimer(event->Handler(), std::move(*args), dt));

	return nullptr;
	}

const CompiledStmt ScheduleExpr::Compile(Compiler* c) const
	{
	IntrusivePtr<ListExpr> event_args = {NewRef{}, event->Args()};
	auto handler = event->Handler();

	bool is_interval = when->Type()->Tag() == TYPE_INTERVAL;

	if ( when->Tag() == EXPR_NAME )
		return c->ScheduleViHL(when->AsNameExpr(), is_interval,
					handler.Ptr(), event_args.get());
	else
		return c->ScheduleCiHL(when->AsConstExpr(), is_interval,
					handler.Ptr(), event_args.get());
	}

IntrusivePtr<Expr> ScheduleExpr::Duplicate()
	{
	auto when_d = when->Duplicate();
	auto event_d = event->Duplicate()->AsEventExprPtr();
	return SetSucc(new ScheduleExpr(when_d, event_d));
	}

IntrusivePtr<Expr> ScheduleExpr::GetOp1() const
	{
	return when;
	}

// We can't inline the following without moving the definition of
// EventExpr in Expr.h to come before that of ScheduleExpr.  Just
// doing this out-of-line seems cleaner.
IntrusivePtr<Expr> ScheduleExpr::GetOp2() const
	{
	return event;
	}

void ScheduleExpr::SetOp1(IntrusivePtr<Expr> op)
	{
	when = op;
	}

void ScheduleExpr::SetOp2(IntrusivePtr<Expr> op)
	{
	event = {NewRef{}, op->AsEventExpr()};
	}

TraversalCode ScheduleExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = when->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = event->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

void ScheduleExpr::ExprDescribe(ODesc* d) const
	{
	if ( d->IsReadable() )
		d->AddSP("schedule");

	when->Describe(d);
	d->SP();

	if ( d->IsReadable() )
		{
		d->Add("{");
		d->PushIndent();
		event->Describe(d);
		d->PopIndent();
		d->Add("}");
		}
	else
		event->Describe(d);
	}

InExpr::InExpr(IntrusivePtr<Expr> arg_op1, IntrusivePtr<Expr> arg_op2)
	: BinaryExpr(EXPR_IN, std::move(arg_op1), std::move(arg_op2))
	{
	if ( IsError() )
		return;

	if ( op1->Type()->Tag() == TYPE_PATTERN )
		{
		if ( op2->Type()->Tag() != TYPE_STRING )
			{
			op2->Type()->Error("pattern requires string index", op1.get());
			SetError();
			}
		else
			SetType(base_type(TYPE_BOOL));
		}

	else if ( op1->Type()->Tag() == TYPE_RECORD )
		{
		if ( op2->Type()->Tag() != TYPE_TABLE )
			{
			op2->Type()->Error("table/set required");
			SetError();
			}

		else
			{
			auto t1 = op1->Type();
			const TypeList* it =
				op2->Type()->AsTableType()->Indices();

			if ( ! same_type(t1.get(), it) )
				{
				t1->Error("indexing mismatch", op2->Type().get());
				SetError();
				}
			else
				SetType(base_type(TYPE_BOOL));
			}
		}

	else if ( op1->Type()->Tag() == TYPE_STRING &&
		  op2->Type()->Tag() == TYPE_STRING )
		SetType(base_type(TYPE_BOOL));

	else
		{
		// Check for:	<addr> in <subnet>
		//		<addr> in set[subnet]
		//		<addr> in table[subnet] of ...
		if ( op1->Type()->Tag() == TYPE_ADDR )
			{
			if ( op2->Type()->Tag() == TYPE_SUBNET )
				{
				SetType(base_type(TYPE_BOOL));
				return;
				}

			if ( op2->Type()->Tag() == TYPE_TABLE &&
			     op2->Type()->AsTableType()->IsSubNetIndex() )
				{
				SetType(base_type(TYPE_BOOL));
				return;
				}
			}

		if ( op1->Tag() != EXPR_LIST )
			op1 = make_intrusive<ListExpr>(std::move(op1));

		ListExpr* lop1 = op1->AsListExpr();

		if ( ! op2->Type()->MatchesIndex(lop1) )
			SetError("not an index type");
		else
			SetType(base_type(TYPE_BOOL));
		}
	}

IntrusivePtr<Val> InExpr::Fold(Val* v1, Val* v2) const
	{
	if ( v1->Type()->Tag() == TYPE_PATTERN )
		{
		RE_Matcher* re = v1->AsPattern();
		const BroString* s = v2->AsString();
		return {AdoptRef{}, val_mgr->GetBool(re->MatchAnywhere(s) != 0)};
		}

	if ( v2->Type()->Tag() == TYPE_STRING )
		{
		const BroString* s1 = v1->AsString();
		const BroString* s2 = v2->AsString();

		// Could do better here e.g. Boyer-Moore if done repeatedly.
		auto s = reinterpret_cast<const unsigned char*>(s1->CheckString());
		auto res = strstr_n(s2->Len(), s2->Bytes(), s1->Len(), s) != -1;
		return {AdoptRef{}, val_mgr->GetBool(res)};
		}

	if ( v1->Type()->Tag() == TYPE_ADDR &&
	     v2->Type()->Tag() == TYPE_SUBNET )
		return {AdoptRef{}, val_mgr->GetBool(v2->AsSubNetVal()->Contains(v1->AsAddr()))};

	bool res;

	if ( is_vector(v2) )
		res = (bool)v2->AsVectorVal()->Lookup(v1);
	else
		res = (bool)v2->AsTableVal()->Lookup(v1, false);

	return {AdoptRef{}, val_mgr->GetBool(res)};
	}

IntrusivePtr<Expr> InExpr::Duplicate()
	{
	auto op1_d = op1->Duplicate();
	auto op2_d = op2->Duplicate();
	return SetSucc(new InExpr(op1_d, op2_d));
	}

bool InExpr::HasReducedOps(Reducer* c) const
	{
	return op1->HasReducedOps(c) && op2->IsSingleton(c);
	}


CallExpr::CallExpr(IntrusivePtr<Expr> arg_func,
                   IntrusivePtr<ListExpr> arg_args, bool in_hook)
	: Expr(EXPR_CALL), func(std::move(arg_func)), args(std::move(arg_args))
	{
	if ( func->IsError() || args->IsError() )
		{
		SetError();
		return;
		}

	auto func_type = func->Type();

	if ( ! IsFunc(func_type->Tag()) )
		{
		func->Error("not a function");
		SetError();
		return;
		}

	if ( func_type->AsFuncType()->Flavor() == FUNC_FLAVOR_HOOK && ! in_hook )
		{
		func->Error("hook cannot be called directly, use hook operator");
		SetError();
		return;
		}

	if ( ! func_type->MatchesIndex(args.get()) )
		SetError("argument type mismatch in function call");

	has_any_arg = false;

	for ( auto a: args->Exprs() )
		{
		auto tag = a->Type()->Tag();
		if ( tag == TYPE_ANY ||
		     (tag == TYPE_VECTOR &&
		      a->Type()->AsVectorType()->YieldType()->Tag() == TYPE_ANY) )
			{
			has_any_arg = true;
			break;
			}
		}

	BroType* yield = func_type->YieldType();

	if ( ! yield )
		{
		switch ( func_type->AsFuncType()->Flavor() ) {

		case FUNC_FLAVOR_FUNCTION:
			Error("function has no yield type");
			SetError();
			break;

		case FUNC_FLAVOR_EVENT:
			Error("event called in expression, use event statement instead");
			SetError();
			break;

		case FUNC_FLAVOR_HOOK:
			Error("hook has no yield type");
			SetError();
			break;

		default:
			Error("invalid function flavor");
			SetError();
			break;
		}
		}
	else
		SetType({NewRef{}, yield});

	// Check for call to built-ins that can be statically analyzed.
	IntrusivePtr<Val> func_val;

	if ( func->Tag() == EXPR_NAME &&
	     // This is cheating, but without it processing gets
	     // quite confused regarding "value used but not set"
	     // run-time errors when we apply this analysis during
	     // parsing.  Really we should instead do it after we've
	     // parsed the entire set of scripts.
	     streq(((NameExpr*) func.get())->Id()->Name(), "fmt") &&
	     // The following is needed because fmt might not yet
	     // be bound as a name.
	     did_builtin_init &&
	     (func_val = func->Eval(nullptr)) )
		{
		::Func* f = func_val->AsFunc();
		if ( f->GetKind() == Func::BUILTIN_FUNC &&
		     ! check_built_in_call((BuiltinFunc*) f, this) )
			SetError();
		}
	}

bool CallExpr::IsPure() const
	{
	if ( IsError() )
		return true;

	if ( ! func->IsPure() )
		return false;

	auto func_val = func->Eval(nullptr);

	if ( ! func_val )
		return false;

	::Func* f = func_val->AsFunc();

	// Only recurse for built-in functions, as recursing on script
	// functions can lead to infinite recursion if the function being
	// called here happens to be recursive (either directly
	// or indirectly).
	bool pure = false;

	if ( f->GetKind() == Func::BUILTIN_FUNC )
		pure = f->IsPure() && args->IsPure();

	return pure;
	}

bool CallExpr::IsReduced(Reducer* c) const
	{
	return func->IsSingleton(c) && args->IsReduced(c);
	}

bool CallExpr::HasReducedOps(Reducer* c) const
	{
	if ( ! func->IsSingleton(c) )
		return NonReduced(this);

	return args->HasReducedOps(c);
	}

Expr* CallExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( c->Optimizing() )
		{
		func = c->UpdateExpr(func);
		auto e = c->UpdateExpr(args);
		auto el = e->AsListExpr();
		args = {NewRef{}, el};
		return this->Ref();
		}

	red_stmt = nullptr;

	if ( ! func->IsSingleton(c) )
		func = {AdoptRef{}, func->ReduceToSingleton(c, red_stmt)};

	IntrusivePtr<Stmt> red2_stmt;
	// We assume that ListExpr won't transform itself fundamentally.
	Unref(args->Reduce(c, red2_stmt));

	// ### could check here for (1) pure function, and (2) all
	// arguments constants, and call it to fold right now.

	red_stmt = MergeStmts(red_stmt, red2_stmt);

	if ( Type()->Tag() == TYPE_VOID )
		return this->Ref();
	else
		return AssignToTemporary(c, red_stmt);
	}

Expr* CallExpr::Inline(Inliner* inl)
	{
	auto new_me = inl->CheckForInlining(this);

	if ( new_me != this )
		return new_me;

	// We're not inlining, but perhaps our elements should be.
	func = {NewRef{}, func->Inline(inl)};

	auto new_args = args->Inline(inl);
	args = {NewRef{}, new_args->AsListExpr()};

	return this->Ref();
	}

IntrusivePtr<Stmt> CallExpr::ReduceToSingletons(Reducer* c)
	{
	IntrusivePtr<Stmt> func_stmt;

	if ( ! func->IsSingleton(c) )
		func = {AdoptRef{}, func->Reduce(c, func_stmt)};

	auto args_stmt = args->ReduceToSingletons(c);

	return MergeStmts(func_stmt, args_stmt);
	}

IntrusivePtr<Val> CallExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return nullptr;

	// If we are inside a trigger condition, we may have already been
	// called, delayed, and then produced a result which is now cached.
	// Check for that.
	if ( f )
		{
		if ( trigger::Trigger* trigger = f->GetTrigger() )
			{
			if ( Val* v = trigger->Lookup(this) )
				{
				DBG_LOG(DBG_NOTIFIERS,
					"%s: provides cached function result",
					trigger->Name());
				return {NewRef{}, v};
				}
			}
		}

	IntrusivePtr<Val> ret;
	auto func_val = func->Eval(f);
	auto v = eval_list(f, args.get());

	if ( func_val && v )
		{
		const ::Func* funcv = func_val->AsFunc();

		if ( has_any_arg )
			{
			// Need to do dynamic type-checking for "any" values.
			auto func_type = func->Type()->AsFuncType();
			auto& f_arg_types = *func_type->ArgTypes()->Types();
			auto& args_e = args->Exprs();

			// We might have more arguments than argument types
			// due to calling a variadic function.  Only compare
			// through the range that's meaningful.
			auto n = std::min(args_e.size(), f_arg_types.size());

			for ( unsigned int i = 0; i < n; ++i )
				{
				auto tag = args_e[i]->Type()->Tag();

				if ( tag != TYPE_ANY &&
				     (tag != TYPE_VECTOR ||
				      args_e[i]->Type()->AsVectorType()->YieldType()->Tag() != TYPE_ANY) )
					// This one isn't interesting.
					continue;

				auto vi_t = (*v)[i]->Type();
				if ( ! same_type(vi_t, f_arg_types[i]) )
					{
					ODesc d;
					vi_t->Describe(&d);
					reporter->RuntimeError(GetLocationInfo(), "type-clash for \"any\" argument, concrete type is %s", d.Description());
					return nullptr;
					}
				}
			}

		const CallExpr* current_call = f ? f->GetCall() : 0;

		if ( f )
			f->SetCall(this);

		ret = funcv->Call(*v, f);

		if ( f )
			f->SetCall(current_call);
		}

	return ret;
	}

const CompiledStmt CallExpr::Compile(Compiler* c) const
	{
	reporter->InternalError("CallExpr::Compile called");
	return c->EmptyStmt();
	}

IntrusivePtr<Expr> CallExpr::Duplicate()
	{
	auto func_d = func->Duplicate();
	auto args_d = args->Duplicate()->AsListExprPtr();
	auto func_type = func->Type();
	auto in_hook = func_type->AsFuncType()->Flavor() == FUNC_FLAVOR_HOOK;

	return SetSucc(new CallExpr(func_d, args_d, in_hook));
	}

TraversalCode CallExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = func->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = args->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

void CallExpr::ExprDescribe(ODesc* d) const
	{
	func->Describe(d);
	if ( d->IsReadable() || d->IsParseable() )
		{
		d->Add("(");
		args->Describe(d);
		d->Add(")");
		}
	else
		args->Describe(d);
	}


InlineExpr::InlineExpr(IntrusivePtr<ListExpr> arg_args, id_list* arg_params,
			IntrusivePtr<Stmt> arg_body, int _frame_offset,
			IntrusivePtr<BroType> t)
: Expr(EXPR_INLINE), args(std::move(arg_args)), body(std::move(arg_body))
	{
	params = arg_params;
	frame_offset = _frame_offset;
	type = t;
	}

bool InlineExpr::IsPure() const
	{
	return args->IsPure() && body->IsPure();
	}

bool InlineExpr::IsReduced(Reducer* c) const
	{
	return NonReduced(this);
	}

Expr* InlineExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	// First, reduce each argument and assign it to a parameter.
	// We do this one at a time because that will often allow the
	// optimizer to collapse the final assignment.

	red_stmt = nullptr;

	auto args_list = args->Exprs();

	loop_over_list(args_list, i)
		{
		IntrusivePtr<Stmt> arg_red_stmt;
		auto red_i = args_list[i]->Reduce(c, arg_red_stmt);
		IntrusivePtr<Expr> red_i_p = {AdoptRef{}, red_i};

		auto param_i = c->GenInlineBlockName((*params)[i]);
		auto assign = make_intrusive<AssignExpr>(param_i, red_i_p,
						false, nullptr, nullptr, false);
		auto assign_stmt = make_intrusive<ExprStmt>(assign);

		red_stmt = MergeStmts(red_stmt, arg_red_stmt, assign_stmt);
		}

	auto ret_val = c->PushInlineBlock(type);
	body = {AdoptRef{}, body->Reduce(c)};
	c->PopInlineBlock();

	auto catch_ret = make_intrusive<CatchReturnStmt>(body, ret_val);

	red_stmt = MergeStmts(red_stmt, catch_ret);

	return ret_val ? ret_val->Duplicate().release() : nullptr;
	}

IntrusivePtr<Val> InlineExpr::Eval(Frame* f) const
	{
	auto v = eval_list(f, args.get());

	if ( ! v )
		return nullptr;

	int nargs = args->Exprs().length();

	f->Reset(frame_offset + nargs);

	f->IncreaseOffset(frame_offset);

	// Assign the arguments.
	for ( auto i = 0; i < nargs; ++i )
		f->SetElement(i, (*v)[i].release());

	stmt_flow_type flow = FLOW_NEXT;
	auto result = body->Exec(f, flow);

	f->IncreaseOffset(-frame_offset);

	return result;
	}

IntrusivePtr<Expr> InlineExpr::Duplicate()
	{
	auto args_d = args->Duplicate()->AsListExprPtr();
	auto body_d = body->Duplicate();
	return SetSucc(new InlineExpr(args_d, params, body_d, frame_offset,
					type));
	}

TraversalCode InlineExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = args->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = body->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

void InlineExpr::ExprDescribe(ODesc* d) const
	{
	if ( d->IsReadable() || d->IsParseable() )
		{
		d->Add("inline(");
		args->Describe(d);
		d->Add("){");
		body->Describe(d);
		d->Add("}");
		}
	else
		{
		args->Describe(d);
		body->Describe(d);
		}
	}

static std::unique_ptr<id_list> shallow_copy_func_inits(const id_list* src)
	{
	if ( ! src )
		return nullptr;

	if ( src->empty() )
		return nullptr;

	auto dest = std::make_unique<id_list>(src->length());

	for ( ID* i : *src )
		{
		Ref(i);
		dest->push_back(i);
		}

	return dest;
	}

LambdaExpr::LambdaExpr(std::unique_ptr<function_ingredients> arg_ing,
                       id_list arg_outer_ids) : Expr(EXPR_LAMBDA)
	{
	ingredients = std::move(arg_ing);
	outer_ids = std::move(arg_outer_ids);

	SetType({NewRef{}, ingredients->id->Type()});

	// Install a dummy version of the function globally for use only
	// when broker provides a closure.
	master_func = make_intrusive<BroFunc>(
		ingredients->id.get(),
		ingredients->body,
		shallow_copy_func_inits(ingredients->inits).release(),
		ingredients->frame_size,
		ingredients->priority);

	master_func->SetOuterIDs(outer_ids);

	analyze_func(master_func.get());

	// Get the body's "string" representation.  Note, even if this
	// representation changes due to subsequent script optimization,
	// what matters is that when building this LambdaExpr AST node,
	// each worker uses the same current representation, since the
	// point is to agree upon an identifier name representing the
	// lambda.
	ODesc d;
	master_func->Describe(&d);

	for ( ; ; )
		{
		uint64_t h[2];
		internal_md5(d.Bytes(), d.Len(), reinterpret_cast<unsigned char*>(h));

		my_name = "lambda_<" + std::to_string(h[0]) + ">";
		auto fullname = make_full_var_name(current_module.data(), my_name.data());
		auto id = global_scope()->Lookup(fullname);

		if ( id )
			// Just try again to make a unique lambda name.  If two peer
			// processes need to agree on the same lambda name, this assumes
			// they're loading the same scripts and thus have the same hash
			// collisions.
			d.Add(" ");
		else
			break;
		}

	// Install that in the global_scope
	auto id = install_ID(my_name.c_str(), current_module.c_str(), true, false);

	// Update lambda's name
	master_func->SetName(my_name.c_str());

	auto v = make_intrusive<Val>(master_func.get());
	id->SetVal(std::move(v));
	id->SetType({NewRef{}, ingredients->id->Type()});
	id->SetConst();
	}

Scope* LambdaExpr::GetScope() const
	{
	return ingredients->scope.get();
	}

IntrusivePtr<Val> LambdaExpr::Eval(Frame* f) const
	{
	// The body and the frame size may have been altered by script
	// optimization, so fetch fresh values for them rather than relying
	// on what was in the original ingredients.
	auto body = master_func->CurrentBody();
	auto frame_size = master_func->FrameSize();

	auto lamb = make_intrusive<BroFunc>(
			ingredients->id.get(), body,
			shallow_copy_func_inits(ingredients->inits).release(),
			frame_size, ingredients->priority);

	lamb->AddClosure(outer_ids, f);

	// Set name to corresponding master func.
	// Allows for lookups by the receiver.
	lamb->SetName(my_name.c_str());

	return make_intrusive<Val>(lamb.get());
	}

Expr* LambdaExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( c->Optimizing() )
		return this->Ref();

	return AssignToTemporary(c, red_stmt);
	}

Expr* LambdaExpr::Inline(Inliner* inl)
	{
	ingredients->body->Inline(inl);
	return this->Ref();
	}

IntrusivePtr<Expr> LambdaExpr::Duplicate()
	{
	auto ingr = std::make_unique<function_ingredients>(*ingredients);
	ingr->body = ingr->body->Duplicate();
	return SetSucc(new LambdaExpr(std::move(ingr), outer_ids));
	}

const CompiledStmt LambdaExpr::Compile(Compiler* c) const
	{
	}

void LambdaExpr::ExprDescribe(ODesc* d) const
	{
	d->Add(expr_name(Tag()));
	master_func->CurrentBody()->Describe(d);
	}

TraversalCode LambdaExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = ingredients->body->Traverse(cb);
	HANDLE_TC_STMT_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

EventExpr::EventExpr(const char* arg_name, IntrusivePtr<ListExpr> arg_args)
	: Expr(EXPR_EVENT), name(arg_name), args(std::move(arg_args))
	{
	EventHandler* h = event_registry->Lookup(name);

	if ( ! h )
		{
		h = new EventHandler(name.c_str());
		event_registry->Register(h);
		}

	h->SetUsed();

	handler = h;

	if ( args->IsError() )
		{
		SetError();
		return;
		}

	FuncType* func_type = h->FType();
	if ( ! func_type )
		{
		Error("not an event");
		SetError();
		return;
		}

	if ( ! func_type->MatchesIndex(args.get()) )
		SetError("argument type mismatch in event invocation");
	else
		{
		if ( func_type->YieldType() )
			{
			Error("function invoked as an event");
			SetError();
			}
		}
	}

IntrusivePtr<Val> EventExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return nullptr;

	auto v = eval_list(f, args.get());

	if ( v )
		mgr.Enqueue(handler, std::move(*v));

	return nullptr;
	}

bool EventExpr::IsReduced(Reducer* c) const
	{
	return Args()->IsReduced(c);
	}

Expr* EventExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	if ( c->Optimizing() )
		{
		auto e = c->UpdateExpr(args);
		auto el = e->AsListExpr();
		args = {NewRef{}, el};
		return this->Ref();
		}

	red_stmt = nullptr;

	if ( ! Args()->IsReduced(c) )
		// We assume that ListExpr won't transform itself fundamentally.
		Unref(Args()->Reduce(c, red_stmt));

	return this->Ref();
	}

IntrusivePtr<Stmt> EventExpr::ReduceToSingletons(Reducer* c)
	{
	return args->ReduceToSingletons(c);
	}

Expr* EventExpr::Inline(Inliner* inl)
	{
	auto e = args->Inline(inl);
	auto el = e->AsListExpr();
	args = {NewRef{}, el};

	return this->Ref();
	}

const CompiledStmt EventExpr::Compile(Compiler* c) const
	{
	EventHandlerPtr p(handler);
	return c->EventHL(p.Ptr(), args.get());
	}

IntrusivePtr<Expr> EventExpr::Duplicate()
	{
	auto args_d = args->Duplicate()->AsListExprPtr();
	return SetSucc(new EventExpr(name.c_str(), args_d));
	}

TraversalCode EventExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = args->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

void EventExpr::ExprDescribe(ODesc* d) const
	{
	d->Add(name.c_str());
	if ( d->IsReadable() || d->IsParseable() )
		{
		d->Add("(");
		args->Describe(d);
		d->Add(")");
		}
	else
		args->Describe(d);
	}

ListExpr::ListExpr() : Expr(EXPR_LIST)
	{
	SetType(make_intrusive<TypeList>());
	}

ListExpr::ListExpr(IntrusivePtr<Expr> e) : Expr(EXPR_LIST)
	{
	SetType(make_intrusive<TypeList>());
	Append(std::move(e));
	}

ListExpr::~ListExpr()
	{
	for ( const auto& expr: exprs )
		Unref(expr);
	}

void ListExpr::Append(IntrusivePtr<Expr> e)
	{
	exprs.push_back(e.release());
	((TypeList*) type.get())->Append(exprs.back()->Type());
	}

bool ListExpr::IsPure() const
	{
	for ( const auto& expr : exprs )
		if ( ! expr->IsPure() )
			return false;

	return true;
	}

bool ListExpr::IsReduced(Reducer* c) const
	{
	for ( const auto& expr : exprs )
		if ( ! expr->IsSingleton(c) )
			{
			if ( expr->Tag() != EXPR_LIST || ! expr->IsReduced(c) )
				return NonReduced(expr);
			}

	return true;
	}

bool ListExpr::HasReducedOps(Reducer* c) const
	{
	for ( const auto& expr : exprs )
		{
		// Ugly hack for record constructors.
		if ( expr->Tag() == EXPR_FIELD_ASSIGN )
			{
			if ( ! expr->HasReducedOps(c) )
				return false;
			}
		else if ( ! expr->IsSingleton(c) )
			return false;
		}

	return true;
	}

IntrusivePtr<Val> ListExpr::Eval(Frame* f) const
	{
	auto v = make_intrusive<ListVal>(TYPE_ANY);

	for ( const auto& expr : exprs )
		{
		auto ev = expr->Eval(f);

		if ( ! ev )
			{
			RuntimeError("uninitialized list value");
			return nullptr;
			}

		v->Append(ev.release());
		}

	return v;
	}

IntrusivePtr<BroType> ListExpr::InitType() const
	{
	if ( exprs.empty() )
		{
		Error("empty list in untyped initialization");
		return nullptr;
		}

	if ( exprs[0]->IsRecordElement(0) )
		{
		type_decl_list* types = new type_decl_list(exprs.length());
		for ( const auto& expr : exprs )
			{
			TypeDecl* td = new TypeDecl(0, 0);
			if ( ! expr->IsRecordElement(td) )
				{
				expr->Error("record element expected");
				delete td;
				delete types;
				return nullptr;
				}

			types->push_back(td);
			}


		return make_intrusive<RecordType>(types);
		}

	else
		{
		auto tl = make_intrusive<TypeList>();

		for ( const auto& e : exprs )
			{
			auto ti = e->Type();

			// Collapse any embedded sets or lists.
			if ( ti->IsSet() || ti->Tag() == TYPE_LIST )
				{
				TypeList* til = ti->IsSet() ?
					ti->AsSetType()->Indices() :
					ti->AsTypeList();

				if ( ! til->IsPure() ||
				     ! til->AllMatch(til->PureType(), true) )
					tl->Append({NewRef{}, til});
				else
					tl->Append({NewRef{}, til->PureType()});
				}
			else
				tl->Append(ti);
			}

		return tl;
		}
	}

IntrusivePtr<Val> ListExpr::InitVal(const BroType* t, IntrusivePtr<Val> aggr) const
	{
	// While fairly similar to the EvalIntoAggregate() code,
	// we keep this separate since it also deals with initialization
	// idioms such as embedded aggregates and cross-product
	// expansion.
	if ( IsError() )
		return nullptr;

	// Check whether each element of this list itself matches t,
	// in which case we should expand as a ListVal.
	if ( ! aggr && type->AsTypeList()->AllMatch(t, true) )
		{
		auto v = make_intrusive<ListVal>(TYPE_ANY);
		const type_list* tl = type->AsTypeList()->Types();

		if ( exprs.length() != tl->length() )
			{
			Error("index mismatch", t);
			return nullptr;
			}

		loop_over_list(exprs, i)
			{
			auto vi = exprs[i]->InitVal((*tl)[i], nullptr);
			if ( ! vi )
				return nullptr;

			v->Append(vi.release());
			}

		return v;
		}

	if ( t->Tag() == TYPE_LIST )
		{
		if ( aggr )
			{
			Error("bad use of list in initialization", t);
			return nullptr;
			}

		const type_list* tl = t->AsTypeList()->Types();

		if ( exprs.length() != tl->length() )
			{
			Error("index mismatch", t);
			return nullptr;
			}

		auto v = make_intrusive<ListVal>(TYPE_ANY);

		loop_over_list(exprs, i)
			{
			auto vi = exprs[i]->InitVal((*tl)[i], nullptr);

			if ( ! vi )
				return nullptr;

			v->Append(vi.release());
			}

		return v;
		}

	if ( t->Tag() != TYPE_RECORD && t->Tag() != TYPE_TABLE &&
	     t->Tag() != TYPE_VECTOR )
		{
		if ( exprs.length() == 1 )
			// Allow "global x:int = { 5 }"
			return exprs[0]->InitVal(t, aggr);
		else
			{
			Error("aggregate initializer for scalar type", t);
			return nullptr;
			}
		}

	if ( ! aggr )
		Internal("missing aggregate in ListExpr::InitVal");

	if ( t->IsSet() )
		return AddSetInit(t, std::move(aggr));

	if ( t->Tag() == TYPE_VECTOR )
		{
		// v: vector = [10, 20, 30];
		VectorVal* vec = aggr->AsVectorVal();

		loop_over_list(exprs, i)
			{
			Expr* e = exprs[i];
			auto promoted_e = check_and_promote_expr(e, vec->Type()->AsVectorType()->YieldType());

			if ( promoted_e )
				e = promoted_e.get();

			if ( ! vec->Assign(i, e->Eval(nullptr)) )
				{
				e->Error(fmt("type mismatch at index %d", i));
				return nullptr;
				}
			}

		return aggr;
		}

	// If we got this far, then it's either a table or record
	// initialization.  Both of those involve AssignExpr's, which
	// know how to add themselves to a table or record.  Another
	// possibility is an expression that evaluates itself to a
	// table, which we can then add to the aggregate.
	for ( const auto& e : exprs )
		{
		if ( e->Tag() == EXPR_ASSIGN || e->Tag() == EXPR_FIELD_ASSIGN )
			{
			if ( ! e->InitVal(t, aggr) )
				return nullptr;
			}
		else
			{
			if ( t->Tag() == TYPE_RECORD )
				{
				e->Error("bad record initializer", t);
				return nullptr;
				}

			auto v = e->Eval(nullptr);

			if ( ! same_type(v->Type(), t) )
				{
				v->Type()->Error("type clash in table initializer", t);
				return nullptr;
				}

			if ( ! v->AsTableVal()->AddTo(aggr->AsTableVal(), true) )
				return nullptr;
			}
		}

	return aggr;
	}

IntrusivePtr<Val> ListExpr::AddSetInit(const BroType* t, IntrusivePtr<Val> aggr) const
	{
	if ( aggr->Type()->Tag() != TYPE_TABLE )
		Internal("bad aggregate in ListExpr::InitVal");

	TableVal* tv = aggr->AsTableVal();
	const TableType* tt = tv->Type()->AsTableType();
	const TypeList* it = tt->Indices();

	for ( const auto& expr : exprs )
		{
		IntrusivePtr<Val> element;

		if ( expr->Type()->IsSet() )
			// A set to flatten.
			element = expr->Eval(nullptr);
		else if ( expr->Type()->Tag() == TYPE_LIST )
			element = expr->InitVal(it, nullptr);
		else
			element = expr->InitVal((*it->Types())[0], nullptr);

		if ( ! element )
			return nullptr;

		if ( element->Type()->IsSet() )
			{
			if ( ! same_type(element->Type(), t) )
				{
				element->Error("type clash in set initializer", t);
				return nullptr;
				}

			if ( ! element->AsTableVal()->AddTo(tv, true) )
				return nullptr;

			continue;
			}

		if ( expr->Type()->Tag() == TYPE_LIST )
			element = check_and_promote(std::move(element), it, true);
		else
			element = check_and_promote(std::move(element), (*it->Types())[0], true);

		if ( ! element )
			return nullptr;

		if ( ! tv->ExpandAndInit(std::move(element), nullptr) )
			return nullptr;
		}

	return aggr;
	}

void ListExpr::ExprDescribe(ODesc* d) const
	{
	d->AddCount(exprs.length());

	if ( ! d->DoOrig() )
		d->Add("<");

	loop_over_list(exprs, i)
		{
		if ( (d->IsReadable() || d->IsParseable()) && i > 0 )
			d->Add(", ");

		exprs[i]->Describe(d);
		}

	if ( ! d->DoOrig() )
		d->Add(">");
	}

IntrusivePtr<Expr> ListExpr::MakeLvalue()
	{
	for ( const auto & expr : exprs )
		if ( expr->Tag() != EXPR_NAME )
			ExprError("can only assign to list of identifiers");

	return make_intrusive<RefExpr>(IntrusivePtr{NewRef{}, this});
	}

void ListExpr::Assign(Frame* f, IntrusivePtr<Val> v)
	{
	ListVal* lv = v->AsListVal();

	if ( exprs.length() != lv->Vals()->length() )
		RuntimeError("mismatch in list lengths");

	loop_over_list(exprs, i)
		exprs[i]->Assign(f, {NewRef{}, (*lv->Vals())[i]});
	}

Expr* ListExpr::Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
	{
	red_stmt = nullptr;

	loop_over_list(exprs, i)
		{
		if ( c->Optimizing() )
			{
			IntrusivePtr<Expr> e_i_ptr = {NewRef{}, exprs[i]};
			auto e_i = c->UpdateExpr(e_i_ptr);
			exprs.replace(i, e_i.get());
			e_i.release();
			continue;
			}

		if ( exprs[i]->IsSingleton(c) )
			continue;

		IntrusivePtr<Stmt> e_stmt;
		exprs.replace(i, exprs[i]->ReduceToSingleton(c, e_stmt));

		if ( e_stmt )
			red_stmt = MergeStmts(red_stmt, e_stmt);
		}

	return this->Ref();
	}

Expr* ListExpr::Inline(Inliner* inl)
	{
	loop_over_list(exprs, i)
		exprs[i] = exprs[i]->Inline(inl);

	return this->Ref();
	}

IntrusivePtr<Stmt> ListExpr::ReduceToSingletons(Reducer* c)
	{
	IntrusivePtr<Stmt> red_stmt;

	loop_over_list(exprs, i)
		{
		if ( exprs[i]->IsSingleton(c) )
			continue;

		IntrusivePtr<Stmt> e_stmt;
		exprs.replace(i, exprs[i]->Reduce(c, e_stmt));

		if ( e_stmt )
			red_stmt = MergeStmts(red_stmt, e_stmt);
		}

	return red_stmt;
	}

IntrusivePtr<Expr> ListExpr::Duplicate()
	{
	auto new_l = new ListExpr();

	loop_over_list(exprs, i)
		new_l->Append(exprs[i]->Duplicate());

	return SetSucc(new_l);
	}

TraversalCode ListExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	for ( const auto& expr : exprs )
		{
		tc = expr->Traverse(cb);
		HANDLE_TC_EXPR_PRE(tc);
		}

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

RecordAssignExpr::RecordAssignExpr(const IntrusivePtr<Expr>& record,
                                   const IntrusivePtr<Expr>& init_list, bool is_init)
	{
	const expr_list& inits = init_list->AsListExpr()->Exprs();

	RecordType* lhs = record->Type()->AsRecordType();

	// The inits have two forms:
	// 1) other records -- use all matching field names+types
	// 2) a string indicating the field name, then (as the next element)
	//    the value to use for that field.

	for ( const auto& init : inits )
		{
		if ( init->Type()->Tag() == TYPE_RECORD )
			{
			RecordType* t = init->Type()->AsRecordType();

			for ( int j = 0; j < t->NumFields(); ++j )
				{
				const char* field_name = t->FieldName(j);
				int field = lhs->FieldOffset(field_name);

				if ( field >= 0 &&
				     same_type(lhs->FieldType(field), t->FieldType(j)) )
					{
					auto fe_lhs = make_intrusive<FieldExpr>(record, field_name);
					auto fe_rhs = make_intrusive<FieldExpr>(IntrusivePtr{NewRef{}, init}, field_name);
					Append(get_assign_expr(std::move(fe_lhs), std::move(fe_rhs), is_init));
					}
				}
			}

		else if ( init->Tag() == EXPR_FIELD_ASSIGN )
			{
			FieldAssignExpr* rf = (FieldAssignExpr*) init;
			rf->Ref();

			const char* field_name = ""; // rf->FieldName();
			if ( lhs->HasField(field_name) )
				{
				auto fe_lhs = make_intrusive<FieldExpr>(record, field_name);
				IntrusivePtr<Expr> fe_rhs = {NewRef{}, rf->Op()};
				Append(get_assign_expr(std::move(fe_lhs), std::move(fe_rhs), is_init));
				}
			else
				{
				string s = "No such field '";
				s += field_name;
				s += "'";
				init_list->SetError(s.c_str());
				}
			}

		else
			{
			init_list->SetError("bad record initializer");
			return;
			}
		}
	}

CastExpr::CastExpr(IntrusivePtr<Expr> arg_op, IntrusivePtr<BroType> t)
	: UnaryExpr(EXPR_CAST, std::move(arg_op))
	{
	auto stype = Op()->Type();

	SetType(std::move(t));

	if ( ! can_cast_value_to_type(stype.get(), Type().get()) )
		ExprError("cast not supported");
	}

IntrusivePtr<Val> CastExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return nullptr;

	auto v = op->Eval(f);

	const char* error;
	auto res = cast_value(v.get(), Type().get(), error);

	if ( error )
		RuntimeError(error);

	return res;
	}

IntrusivePtr<Expr> CastExpr::Duplicate()
	{
	return SetSucc(new CastExpr(op->Duplicate(), type));
	}

IntrusivePtr<Val> cast_value(Val* v, BroType* t, const char*& error)
	{
	error = nullptr;

	auto nv = cast_value_to_type(v, t);

	if ( nv )
		return nv;

	static ODesc d;
	d.Clear();

	d.Add("invalid cast of value with type '");
	v->Type()->Describe(&d);
	d.Add("' to type '");
	t->Describe(&d);
	d.Add("'");

	if ( same_type(v->Type(), bro_broker::DataVal::ScriptDataType()) &&
		 ! v->AsRecordVal()->Lookup(0) )
		d.Add(" (nil $data field)");

	error = d.Description();
	return nullptr;
	}

void CastExpr::ExprDescribe(ODesc* d) const
	{
	Op()->Describe(d);
	d->Add(" as ");
	Type()->Describe(d);
	}

IsExpr::IsExpr(IntrusivePtr<Expr> arg_op, IntrusivePtr<BroType> arg_t)
	: UnaryExpr(EXPR_IS, std::move(arg_op)), t(std::move(arg_t))
	{
	SetType(base_type(TYPE_BOOL));
	}

IntrusivePtr<Val> IsExpr::Fold(Val* v) const
	{
	if ( IsError() )
		return nullptr;

	return {AdoptRef{}, val_mgr->GetBool(can_cast_value_to_type(v, t.get()))};
	}

IntrusivePtr<Expr> IsExpr::Duplicate()
	{
	return SetSucc(new IsExpr(op->Duplicate(), t));
	}

void IsExpr::ExprDescribe(ODesc* d) const
	{
	Op()->Describe(d);
	d->Add(" is ");
	t->Describe(d);
	}

void NopExpr::ExprDescribe(ODesc* d) const
	{
	if ( d->IsReadable() )
		d->Add("NOP");
	}

IntrusivePtr<Val> NopExpr::Eval(Frame* /* f */) const
	{
	return nullptr;
	}

TraversalCode NopExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

IntrusivePtr<Expr> get_assign_expr(IntrusivePtr<Expr> op1,
                                   IntrusivePtr<Expr> op2, bool is_init)
	{
	if ( op1->Type()->Tag() == TYPE_RECORD &&
	     op2->Type()->Tag() == TYPE_LIST )
		return make_intrusive<RecordAssignExpr>(std::move(op1), std::move(op2),
		                                        is_init);

	else if ( op1->Tag() == EXPR_INDEX && op1->AsIndexExpr()->IsSlice() )
		return make_intrusive<IndexSliceAssignExpr>(std::move(op1),
		                                            std::move(op2), is_init);

	else
		return make_intrusive<AssignExpr>(std::move(op1), std::move(op2),
		                                  is_init);
	}

IntrusivePtr<Expr> get_temp_assign_expr(IntrusivePtr<Expr> op1,
					   IntrusivePtr<Expr> op2)
	{
	return make_intrusive<AssignExpr>(std::move(op1), std::move(op2),
						false, nullptr, nullptr, false);
	}

IntrusivePtr<Expr> check_and_promote_expr(Expr* const e, BroType* t)
	{
	auto et = e->Type();
	TypeTag e_tag = et->Tag();
	TypeTag t_tag = t->Tag();

	if ( t_tag == TYPE_ANY )
		{
		IntrusivePtr<Expr> e_ptr = {NewRef{}, e};

		if ( e_tag == TYPE_ANY )
			return e_ptr;
		else
			return make_intrusive<CoerceToAnyExpr>(e_ptr);
		}

	if ( e_tag == TYPE_ANY )
		{
		IntrusivePtr<Expr> e_ptr = {NewRef{}, e};
		IntrusivePtr<BroType> t_ptr = {NewRef{}, t};
		return make_intrusive<CoerceFromAnyExpr>(e_ptr, t_ptr);
		}

	if ( EitherArithmetic(t_tag, e_tag) )
		{
		if ( e_tag == t_tag )
			return {NewRef{}, e};

		if ( ! BothArithmetic(t_tag, e_tag) )
			{
			t->Error("arithmetic mixed with non-arithmetic", e);
			return nullptr;
			}

		TypeTag mt = max_type(t_tag, e_tag);
		if ( mt != t_tag )
			{
			t->Error("over-promotion of arithmetic value", e);
			return nullptr;
			}

		return make_intrusive<ArithCoerceExpr>(IntrusivePtr{NewRef{}, e}, t_tag);
		}

	if ( t->Tag() == TYPE_RECORD && et->Tag() == TYPE_RECORD )
		{
		RecordType* t_r = t->AsRecordType();
		RecordType* et_r = et->AsRecordType();

		if ( same_type(t, et.get()) )
			{
			// Make sure the attributes match as well.
			for ( int i = 0; i < t_r->NumFields(); ++i )
				{
				const TypeDecl* td1 = t_r->FieldDecl(i);
				const TypeDecl* td2 = et_r->FieldDecl(i);

				if ( same_attrs(td1->attrs.get(), td2->attrs.get()) )
					// Everything matches perfectly.
					return {NewRef{}, e};
				}
			}

		if ( record_promotion_compatible(t_r, et_r) )
			return make_intrusive<RecordCoerceExpr>(IntrusivePtr{NewRef{}, e},
			                                        IntrusivePtr{NewRef{}, t_r});

		t->Error("incompatible record types", e);
		return nullptr;
		}


	if ( ! same_type(t, et.get()) )
		{
		if ( t->Tag() == TYPE_TABLE && et->Tag() == TYPE_TABLE &&
			  et->AsTableType()->IsUnspecifiedTable() )
			return make_intrusive<TableCoerceExpr>(IntrusivePtr{NewRef{}, e},
			                                       IntrusivePtr{NewRef{}, t->AsTableType()});

		if ( t->Tag() == TYPE_VECTOR && et->Tag() == TYPE_VECTOR &&
		     et->AsVectorType()->IsUnspecifiedVector() )
			return make_intrusive<VectorCoerceExpr>(IntrusivePtr{NewRef{}, e},
			                                        IntrusivePtr{NewRef{}, t->AsVectorType()});

		t->Error("type clash", e);
		return nullptr;
		}

	return {NewRef{}, e};
	}

bool check_and_promote_exprs(ListExpr* const elements, TypeList* types)
	{
	expr_list& el = elements->Exprs();
	const type_list* tl = types->Types();

	if ( tl->length() == 1 && (*tl)[0]->Tag() == TYPE_ANY )
		// VP: what is this special case for?  It doesn't
		// appear to be accessed during parsing of the default
		// script base.
		return true;

	if ( el.length() != tl->length() )
		{
		types->Error("indexing mismatch", elements);
		return false;
		}

	loop_over_list(el, i)
		{
		Expr* e = el[i];
		auto promoted_e = check_and_promote_expr(e, (*tl)[i]);

		if ( ! promoted_e )
			{
			e->Error("type mismatch", (*tl)[i]);
			return false;
			}

		if ( promoted_e.get() != e )
			{
			Unref(e);
			el.replace(i, promoted_e.release());
			}
		}

	return true;
	}

bool check_and_promote_args(ListExpr* const args, RecordType* types)
	{
	expr_list& el = args->Exprs();
	int ntypes = types->NumFields();

	// give variadic BIFs automatic pass
	if ( ntypes == 1 && types->FieldDecl(0)->type->Tag() == TYPE_ANY )
		return true;

	if ( el.length() < ntypes )
		{
		expr_list def_elements;

		// Start from rightmost parameter, work backward to fill in missing
		// arguments using &default expressions.
		for ( int i = ntypes - 1; i >= el.length(); --i )
			{
			TypeDecl* td = types->FieldDecl(i);
			Attr* def_attr = td->attrs ? td->attrs->FindAttr(ATTR_DEFAULT) : 0;

			if ( ! def_attr )
				{
				types->Error("parameter mismatch", args);
				return false;
				}

			// Don't use the default expression directly, as
			// doing so will wind up sharing its code across
			// different invocations that use the default
			// argument.  That works okay for the interpreter,
			// but if we transform the code we want that done
			// separately for each instance, rather than
			// one instance inheriting the transformed version
			// from another.
			auto e = def_attr->AttrExpr();
			def_elements.push_front(e->Duplicate().release());
			}

		for ( const auto& elem : def_elements )
			el.push_back(elem->Ref());
		}

	TypeList* tl = new TypeList();

	for ( int i = 0; i < types->NumFields(); ++i )
		tl->Append({NewRef{}, types->FieldType(i)});

	int rval = check_and_promote_exprs(args, tl);
	Unref(tl);

	return rval;
	}

bool check_and_promote_exprs_to_type(ListExpr* const elements, BroType* type)
	{
	expr_list& el = elements->Exprs();

	if ( type->Tag() == TYPE_ANY )
		return true;

	loop_over_list(el, i)
		{
		Expr* e = el[i];
		auto promoted_e = check_and_promote_expr(e, type);

		if ( ! promoted_e )
			{
			e->Error("type mismatch", type);
			return false;
			}

		if ( promoted_e.get() != e )
			{
			Unref(e);
			el.replace(i, promoted_e.release());
			}
		}

	return true;
	}

std::optional<std::vector<IntrusivePtr<Val>>> eval_list(Frame* f, const ListExpr* l)
	{
	const expr_list& e = l->Exprs();
	auto rval = std::make_optional<std::vector<IntrusivePtr<Val>>>();
	rval->reserve(e.length());

	for ( const auto& expr : e )
		{
		auto ev = expr->Eval(f);

		if ( ! ev )
			return {};

		rval->emplace_back(std::move(ev));
		}

	return rval;
	}

bool same_singletons(IntrusivePtr<Expr> e1, IntrusivePtr<Expr> e2)
	{
	auto e1t = e1->Tag();
	auto e2t = e2->Tag();

	if ( (e1t != EXPR_NAME && e1t != EXPR_CONST) ||
	     (e2t != EXPR_NAME && e2t != EXPR_CONST) )
		return false;

	if ( e1t != e2t )
		return false;

	if ( e1t == EXPR_CONST )
		{
		auto c1 = e1->AsConstExpr()->Value();
		auto c2 = e2->AsConstExpr()->Value();

		if ( ! is_atomic_val(c1) || ! is_atomic_val(c2) )
			return false;

		return same_atomic_val(c1, c2);
		}

	auto i1 = e1->AsNameExpr()->Id();
	auto i2 = e2->AsNameExpr()->Id();

	return i1 == i2;
	}

bool expr_greater(const Expr* e1, const Expr* e2)
	{
	return e1->Tag() > e2->Tag();
	}
