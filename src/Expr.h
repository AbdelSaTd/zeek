// See the file "COPYING" in the main distribution directory for copyright.

#pragma once

#include "BroList.h"
#include "IntrusivePtr.h"
#include "StmtBase.h"
#include "ID.h"
#include "Timer.h"
#include "Type.h"
#include "EventHandler.h"
#include "TraverseTypes.h"
#include "Val.h"
#include "ZeekArgs.h"

#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <optional>

using std::string;

enum BroExprTag : int {
	EXPR_ANY = -1,
	EXPR_NAME, EXPR_CONST,
	EXPR_CLONE,
	EXPR_INCR, EXPR_DECR,
	EXPR_NOT, EXPR_COMPLEMENT,
	EXPR_POSITIVE, EXPR_NEGATE,
	EXPR_ADD, EXPR_SUB, EXPR_ADD_TO, EXPR_APPEND_TO, EXPR_REMOVE_FROM,
	EXPR_TIMES, EXPR_DIVIDE, EXPR_MOD,
	EXPR_AND, EXPR_OR, EXPR_XOR,
	EXPR_AND_AND, EXPR_OR_OR,
	EXPR_LT, EXPR_LE, EXPR_EQ, EXPR_NE, EXPR_GE, EXPR_GT,
	EXPR_COND,
	EXPR_REF,
	EXPR_ASSIGN, EXPR_INDEX_ASSIGN, EXPR_FIELD_LHS_ASSIGN,
	EXPR_INDEX, EXPR_ANY_INDEX,
	EXPR_FIELD, EXPR_HAS_FIELD,
	EXPR_RECORD_CONSTRUCTOR,
	EXPR_TABLE_CONSTRUCTOR,
	EXPR_SET_CONSTRUCTOR,
	EXPR_VECTOR_CONSTRUCTOR,
	EXPR_FIELD_ASSIGN,
	EXPR_IN,
	EXPR_LIST,
	EXPR_CALL,
	EXPR_INLINE,
	EXPR_LAMBDA,
	EXPR_EVENT,
	EXPR_SCHEDULE,
	EXPR_ARITH_COERCE,
	EXPR_RECORD_COERCE,
	EXPR_TABLE_COERCE,
	EXPR_VECTOR_COERCE,
	EXPR_TO_ANY_COERCE, EXPR_FROM_ANY_COERCE,
	EXPR_SIZE,
	EXPR_CAST,
	EXPR_IS,
	EXPR_INDEX_SLICE_ASSIGN,
	EXPR_NOP,
#define NUM_EXPRS (int(EXPR_NOP) + 1)
};

// Second argument specifies whether we want the name for a human-readable
// "describe".  This suppresses rendering for some internal operations.
extern const char* expr_name(BroExprTag t, bool is_describe = false);

template <class T> class IntrusivePtr;
class Stmt;
class Frame;
class Scope;
class ListExpr;
class NameExpr;
class IndexExpr;
class AssignExpr;
class IndexAssignExpr;
class AnyIndexExpr;
class FieldLHSAssignExpr;
class FieldExpr;
class HasFieldExpr;
class FieldAssignExpr;
class CallExpr;
class InlineExpr;
class EventExpr;
class RefExpr;
class IsExpr;
class IncrExpr;
class AddToExpr;
class AppendToExpr;
class RemoveFromExpr;
class NegExpr;
class ConstExpr;
class CondExpr;
class RecordCoerceExpr;
class TableConstructorExpr;
class SetConstructorExpr;
class RecordConstructorExpr;

struct function_ingredients;

class Reducer;
class Inliner;
class IfStmt;
class CompiledStmt;
class Compiler;
class ZAM;
class ZBody;


class Expr : public BroObj {
public:
	~Expr()	{ Unref(original); }

	IntrusivePtr<BroType> Type() const		{ return type; }
	BroExprTag Tag() const	{ return tag; }

	Expr* Ref()			{ ::Ref(this); return this; }

	// Evaluates the expression and returns a corresponding Val*,
	// or nil if the expression's value isn't fixed.
	virtual IntrusivePtr<Val> Eval(Frame* f) const = 0;

	// Same, but the context is that we are adding an element
	// into the given aggregate of the given type.  Note that
	// return type is void since it's updating an existing
	// value, rather than creating a new one.
	virtual void EvalIntoAggregate(const BroType* t, Val* aggr, Frame* f)
			const;

	// Assign to the given value, if appropriate.
	virtual void Assign(Frame* f, IntrusivePtr<Val> v);

	// Helper function for factoring out index-based assignment.
	void AssignToIndex(IntrusivePtr<Val> v1, IntrusivePtr<Val> v2,
				IntrusivePtr<Val> v3) const;

	// Returns a new expression corresponding to a temporary
	// that's been assigned to the given expression via red_stmt.
	Expr* AssignToTemporary(Expr* e, Reducer* c,
				IntrusivePtr<Stmt>& red_stmt);
	// Same but for this expression.
	Expr* AssignToTemporary(Reducer* c, IntrusivePtr<Stmt>& red_stmt)
		{ return AssignToTemporary(this, c, red_stmt); }

	// Returns the type corresponding to this expression interpreted
	// as an initialization.  Returns nil if the initialization is illegal.
	virtual IntrusivePtr<BroType> InitType() const;

	// Returns true if this expression, interpreted as an initialization,
	// constitutes a record element, false otherwise.  If the TypeDecl*
	// is non-nil and the expression is a record element, fills in the
	// TypeDecl with a description of the element.
	virtual bool IsRecordElement(TypeDecl* td) const;

	// Returns a value corresponding to this expression interpreted
	// as an initialization, or nil if the expression is inconsistent
	// with the given type.  If "aggr" is non-nil, then this expression
	// is an element of the given aggregate, and it is added to it
	// accordingly.
	virtual IntrusivePtr<Val> InitVal(const BroType* t, IntrusivePtr<Val> aggr) const;

	// This used to be framed as "true if the expression has no side
	// effects, false otherwise", but that's not quite right, since
	// for identifiers it's only true if they're constant.  So this
	// is more like "has no variable elements".
	virtual bool IsPure() const;

	// True if the expression has no side effects, false otherwise.
	virtual bool HasNoSideEffects() const	{ return IsPure(); }

	// True if the expression is in fully reduced form: a singleton
	// or an assignment to an operator with singleton operands.
	virtual bool IsReduced(Reducer* c) const;

	// True if the expression's operands are singletons.
	virtual bool HasReducedOps(Reducer* c) const;

	// True if the expression is reduced to a form that can be
	// used in a conditional.
	bool IsReducedConditional(Reducer* c) const;

	// True if the expression is reduced to a form that can be
	// used in a field assignment.
	bool IsReducedFieldAssignment(Reducer* c) const;
	bool IsFieldAssignable(const Expr* e) const;

	// Returns a set of predecessor statements in red_stmt (which might
	// be nil if no reduction necessary), and the reduced version of
	// the expression, suitable for replacing previous uses.  The
	// second version always yields a singleton suitable for use
	// as an operand.  The first version does this too except
	// for assignment statements; thus, its form is not guarantee
	// suitable for use as an operand.
	virtual Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt);
	virtual Expr* ReduceToSingleton(Reducer* c,
					IntrusivePtr<Stmt>& red_stmt)
		{ return Reduce(c, red_stmt); }

	// Reduces the expression to one whose operands are singletons.
	// Returns a predecessor statement(list), if any.
	virtual IntrusivePtr<Stmt> ReduceToSingletons(Reducer* c);

	// Reduces the expression to one that can appear as a conditional.
	Expr* ReduceToConditional(Reducer* c, IntrusivePtr<Stmt>& red_stmt);

	// Reduces the expression to one that can appear as a field
	// assignment.
	Expr* ReduceToFieldAssignment(Reducer* c, IntrusivePtr<Stmt>& red_stmt);

	virtual Expr* Inline(Inliner* inl)	{ return this->Ref(); }

	virtual const CompiledStmt Compile(Compiler* c) const;

	// Returns a duplciate of the expression.  For atomic expressions
	// that can be safely shared across multiple function bodies
	// (due to inline-ing), and that won't have Reaching Definitions
	// tied to an individual copy, we can return just a reference, per the
	// default here.
	virtual IntrusivePtr<Expr> Duplicate()
		{ return {NewRef{}, this}; }

	// True if the expression can serve as an operand to a reduced
	// expression.
	bool IsSingleton(Reducer* r) const
		{
		return (tag == EXPR_NAME && IsReduced(r)) || tag == EXPR_CONST;
		}

	// True if the expression is a constant, false otherwise.
	bool IsConst() const	{ return tag == EXPR_CONST; }

	// If the expression always evaluates to the same value, returns
	// that value.  Otherwise, returns nullptr.
	virtual IntrusivePtr<Val> FoldVal() const	{ return nullptr; }

	bool HasConstantOps() const
		{
		return GetOp1() && GetOp1()->IsConst() &&
			(! GetOp2() ||
			 (GetOp2()->IsConst() &&
			  (! GetOp3() || GetOp3()->IsConst())));
		}

	// True if the expression will transform to one of another type
	// upon reduction, for non-constant operands.  "Transform" means
	// something beyond assignment to a temporary.  Necessary so that
	// we know to fully reduce such expressions if they're the RHS
	// of an assignment.
	virtual bool WillTransform(Reducer* c) const	{ return false; }

	// The same, but for the expression used in a conditional context.
	virtual bool WillTransformInConditional(Reducer* c) const
		{ return false; }

	// True if the expression is in error (to alleviate error propagation).
	bool IsError() const;

	// Mark expression as in error.
	void SetError();
	void SetError(const char* msg);

	// Returns the expression's constant value, or complains
	// if it's not a constant.
	inline Val* ExprVal() const;

	// Returns the expressions operands, or nil if it doesn't
	// have one.
	virtual IntrusivePtr<Expr> GetOp1() const;
	virtual IntrusivePtr<Expr> GetOp2() const;
	virtual IntrusivePtr<Expr> GetOp3() const;

	// Sets the operands to new values.
	virtual void SetOp1(IntrusivePtr<Expr> new_op);
	virtual void SetOp2(IntrusivePtr<Expr> new_op);
	virtual void SetOp3(IntrusivePtr<Expr> new_op);

	// Helper function to reduce boring code runs.
	IntrusivePtr<Stmt> MergeStmts(IntrusivePtr<Stmt> s1,
					IntrusivePtr<Stmt> s2,
					IntrusivePtr<Stmt> s3 = nullptr) const;

	// True if the expression is a constant zero, false otherwise.
	bool IsZero() const;

	// True if the expression is a constant one, false otherwise.
	bool IsOne() const;

	// True if the expression supports the "add" or "delete" operations,
	// false otherwise.
	virtual bool CanAdd() const;
	virtual bool CanDel() const;

	virtual void Add(Frame* f);	// perform add operation
	virtual void Delete(Frame* f);	// perform delete operation

	// Return the expression converted to L-value form.  If expr
	// cannot be used as an L-value, reports an error and returns
	// the current value of expr (this is the default method).
	virtual IntrusivePtr<Expr> MakeLvalue();

	// Marks the expression as one requiring (or at least appearing
	// with) parentheses.  Used for pretty-printing.
	void MarkParen()		{ paren = true; }
	bool IsParen() const		{ return paren; }

#undef ACCESSOR
#define ACCESSOR(tag, ctype, name) \
        ctype* name() \
                { \
                CHECK_TAG(Tag(), tag, "Expr::ACCESSOR", expr_name) \
                return (ctype*) this; \
                }

#undef PTR_ACCESSOR
#define PTR_ACCESSOR(tag, ctype, name) \
        IntrusivePtr<ctype> name ## Ptr() \
                { \
                CHECK_TAG(Tag(), tag, "Expr::ACCESSOR", expr_name) \
		IntrusivePtr<ctype> res = {NewRef{}, (ctype*) this}; \
                return res; \
                }

#undef CONST_ACCESSOR
#define CONST_ACCESSOR(tag, ctype, name) \
        const ctype* name() const \
                { \
                CHECK_TAG(Tag(), tag, "Expr::CONST_ACCESSOR", expr_name) \
                return (const ctype*) this; \
                }

#undef ACCESSORS
#define ACCESSORS(tag, ctype, name) \
	ACCESSOR(tag, ctype, name) \
	PTR_ACCESSOR(tag, ctype, name) \
	CONST_ACCESSOR(tag, ctype, name)

	ACCESSORS(EXPR_LIST, ListExpr, AsListExpr);
	ACCESSORS(EXPR_NAME, NameExpr, AsNameExpr);
	ACCESSORS(EXPR_CONST, ConstExpr, AsConstExpr);
	ACCESSORS(EXPR_ASSIGN, AssignExpr, AsAssignExpr);
	ACCESSORS(EXPR_INDEX_ASSIGN, IndexAssignExpr, AsIndexAssignExpr);
	ACCESSORS(EXPR_ANY_INDEX, AnyIndexExpr, AsAnyIndexExpr);
	ACCESSORS(EXPR_FIELD_LHS_ASSIGN, FieldLHSAssignExpr, AsFieldLHSAssignExpr);
	ACCESSORS(EXPR_FIELD, FieldExpr, AsFieldExpr);
	ACCESSORS(EXPR_FIELD_ASSIGN, FieldAssignExpr, AsFieldAssignExpr);
	ACCESSORS(EXPR_INDEX, IndexExpr, AsIndexExpr);
	ACCESSORS(EXPR_REF, RefExpr, AsRefExpr);
	ACCESSORS(EXPR_EVENT, EventExpr, AsEventExpr);
	ACCESSORS(EXPR_RECORD_COERCE, RecordCoerceExpr, AsRecordCoerceExpr);
	ACCESSORS(EXPR_TABLE_CONSTRUCTOR, TableConstructorExpr, AsTableConstructorExpr);
	ACCESSORS(EXPR_SET_CONSTRUCTOR, SetConstructorExpr, AsSetConstructorExpr);
	ACCESSORS(EXPR_RECORD_CONSTRUCTOR, RecordConstructorExpr, AsRecordConstructorExpr);

	CONST_ACCESSOR(EXPR_HAS_FIELD, HasFieldExpr, AsHasFieldExpr);
	CONST_ACCESSOR(EXPR_CALL, CallExpr, AsCallExpr);
	CONST_ACCESSOR(EXPR_INLINE, InlineExpr, AsInlineExpr);
	CONST_ACCESSOR(EXPR_ADD_TO, AddToExpr, AsAddToExpr);
	CONST_ACCESSOR(EXPR_INCR, IncrExpr, AsIncrExpr);
	CONST_ACCESSOR(EXPR_APPEND_TO, AppendToExpr, AsAppendToExpr);
	CONST_ACCESSOR(EXPR_COND, CondExpr, AsCondExpr);
	CONST_ACCESSOR(EXPR_IS, IsExpr, AsIsExpr);

#undef ACCESSORS
#undef ACCESSOR
#undef CONST_ACCESSOR

	void SetOriginal(Expr* _orig)
		{
		if ( ! original )
			original = _orig->Ref();
		}

	const Expr* Original() const
		{
		if ( original )
			return original->Original();
		else
			return this;
		}

	IntrusivePtr<Expr> SetSucc(Expr* succ)
		{
		succ->SetOriginal(this);
		return {AdoptRef{}, succ};
		}

	void Describe(ODesc* d) const override;

	virtual TraversalCode Traverse(TraversalCallback* cb) const = 0;

protected:
	friend ZAM;
	friend ZBody;

	// The following doesn't appear to be used.
	// Expr() = default;
	explicit Expr(BroExprTag arg_tag);

	void SeatBelts(const BroType* t1, const BroType* t2) const;
	void SeatBelts(const BroType* t1, IntrusivePtr<BroType> t2) const
		{ SeatBelts(t1, t2.get()); }
	void SeatBelts(IntrusivePtr<BroType> t1, const BroType* t2) const
		{ SeatBelts(t1.get(), t2); }
	void SeatBelts(IntrusivePtr<BroType> t1, IntrusivePtr<BroType> t2) const
		{ SeatBelts(t1.get(), t2.get()); }

	Val* MakeZero(TypeTag t) const;
	ConstExpr* MakeZeroExpr(TypeTag t) const;

	virtual void ExprDescribe(ODesc* d) const = 0;
	void AddTag(ODesc* d) const;

	// Puts the expression in canonical form.
	virtual void Canonicize();

	Expr* TransformMe(Expr* new_me, Reducer* c,
				IntrusivePtr<Stmt>& red_stmt);

	void SetType(IntrusivePtr<BroType> t);

	// Reports the given error and sets the expression's type to
	// TYPE_ERROR.
	void ExprError(const char msg[]);

	// These two functions both call Reporter::RuntimeError or Reporter::ExprRuntimeError,
	// both of which are marked as [[noreturn]].
	[[noreturn]] void RuntimeError(const std::string& msg) const;
	[[noreturn]] void RuntimeErrorWithCallStack(const std::string& msg) const;

	// The following is to allow IfStmt::Compile to directly alter
	// the tag of certain conditional statements.
	friend class IfStmt;

	Expr* original;
	BroExprTag tag;
	IntrusivePtr<BroType> type;
	bool paren;
};

extern const char* assign_to_index(IntrusivePtr<Val> v1, IntrusivePtr<Val> v2,
					IntrusivePtr<Val> v3);

class NameExpr : public Expr {
public:
	explicit NameExpr(IntrusivePtr<ID> id, bool const_init = false);

	ID* Id() const			{ return id.get(); }
	IntrusivePtr<ID> IdPtr() const	{ return id; }

	IntrusivePtr<Val> FoldVal() const override;
	IntrusivePtr<Val> Eval(Frame* f) const override;
	void Assign(Frame* f, IntrusivePtr<Val> v) override;
	IntrusivePtr<Expr> MakeLvalue() override;
	bool IsPure() const override;
	bool HasReducedOps(Reducer* c) const override	{ return IsReduced(c); }
	bool HasNoSideEffects() const override	{ return true; }

	bool IsReduced(Reducer* c) const override;
	bool WillTransform(Reducer* c) const override
		{ return ! IsReduced(c); }
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<Expr> Duplicate() override;

	TraversalCode Traverse(TraversalCallback* cb) const override;

protected:
	// Returns true if our identifier is a global with a constant value
	// that can be propagated.
	bool FoldableGlobal() const;

	void ExprDescribe(ODesc* d) const override;

	IntrusivePtr<ID> id;
	bool in_const_init;
};

class ConstExpr : public Expr {
public:
	explicit ConstExpr(IntrusivePtr<Val> val);

	Val* Value() const	{ return val.get(); }
	IntrusivePtr<Val> ValuePtr() const	{ return val; }

	IntrusivePtr<Val> FoldVal() const override	{ return val; }

	IntrusivePtr<Val> Eval(Frame* f) const override;

	TraversalCode Traverse(TraversalCallback* cb) const override;

protected:
	void ExprDescribe(ODesc* d) const override;
	IntrusivePtr<Val> val;
};

class UnaryExpr : public Expr {
public:
	Expr* Op() const	{ return op.get(); }

	IntrusivePtr<Expr> GetOp1() const override final	{ return op; }
	void SetOp1(IntrusivePtr<Expr> _op) override final { op = _op; }

	// UnaryExpr::Eval correctly handles vector types.  Any child
	// class that overrides Eval() should be modified to handle
	// vectors correctly as necessary.
	IntrusivePtr<Val> Eval(Frame* f) const override;

	bool IsPure() const override;
	bool HasNoSideEffects() const override;
	bool IsReduced(Reducer* c) const override;
	bool HasReducedOps(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
	Expr* Inline(Inliner* inl) override;

	TraversalCode Traverse(TraversalCallback* cb) const override;

protected:
	UnaryExpr(BroExprTag arg_tag, IntrusivePtr<Expr> arg_op);

	void ExprDescribe(ODesc* d) const override;

	// Returns the expression folded using the given constant.
	virtual IntrusivePtr<Val> Fold(Val* v) const;

	IntrusivePtr<Expr> op;
};

class BinaryExpr : public Expr {
public:
	Expr* Op1() const	{ return op1.get(); }
	Expr* Op2() const	{ return op2.get(); }

	IntrusivePtr<Expr> GetOp1() const override final	{ return op1; }
	IntrusivePtr<Expr> GetOp2() const override final	{ return op2; }

	void SetOp1(IntrusivePtr<Expr> _op) override final { op1 = _op; }
	void SetOp2(IntrusivePtr<Expr> _op) override final { op2 = _op; }

	bool IsPure() const override;
	bool HasNoSideEffects() const override;
	bool IsReduced(Reducer* c) const override;
	bool HasReducedOps(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
	Expr* Inline(Inliner* inl) override;

	// BinaryExpr::Eval correctly handles vector types.  Any child
	// class that overrides Eval() should be modified to handle
	// vectors correctly as necessary.
	IntrusivePtr<Val> Eval(Frame* f) const override;

	TraversalCode Traverse(TraversalCallback* cb) const override;

protected:
	BinaryExpr(BroExprTag arg_tag,
	           IntrusivePtr<Expr> arg_op1, IntrusivePtr<Expr> arg_op2)
		: Expr(arg_tag), op1(std::move(arg_op1)), op2(std::move(arg_op2))
		{
		if ( ! (op1 && op2) )
			return;
		if ( op1->IsError() || op2->IsError() )
			SetError();
		}

	// Returns the expression folded using the given constants.
	virtual IntrusivePtr<Val> Fold(Val* v1, Val* v2) const;

	// Same for when the constants are strings.
	virtual IntrusivePtr<Val> StringFold(Val* v1, Val* v2) const;

	// Same for when the constants are patterns.
	virtual IntrusivePtr<Val> PatternFold(Val* v1, Val* v2) const;

	// Same for when the constants are sets.
	virtual IntrusivePtr<Val> SetFold(Val* v1, Val* v2) const;

	// Same for when the constants are addresses or subnets.
	virtual IntrusivePtr<Val> AddrFold(Val* v1, Val* v2) const;
	virtual IntrusivePtr<Val> SubNetFold(Val* v1, Val* v2) const;

	bool BothConst() const	{ return op1->IsConst() && op2->IsConst(); }

	// Exchange op1 and op2.
	void SwapOps();

	// Promote the operands to the given type tag, if necessary.
	void PromoteOps(TypeTag t);

	// Promote the expression to the given type tag (i.e., promote
	// operands and also set expression's type).
	void PromoteType(TypeTag t, bool is_vector);

	// Promote one of the operands to be "double" (if not already),
	// to make it suitable for combining with the other "interval"
	// operand, yielding an "interval" type.
	void PromoteForInterval(IntrusivePtr<Expr>& op);

	void ExprDescribe(ODesc* d) const override;

	IntrusivePtr<Expr> op1;
	IntrusivePtr<Expr> op2;
};

class CloneExpr : public UnaryExpr {
public:
	explicit CloneExpr(IntrusivePtr<Expr> op);
	IntrusivePtr<Val> Eval(Frame* f) const override;

protected:
	IntrusivePtr<Val> Fold(Val* v) const override;

	IntrusivePtr<Expr> Duplicate() override;
};

class IncrExpr : public UnaryExpr {
public:
	IncrExpr(BroExprTag tag, IntrusivePtr<Expr> op);

	IntrusivePtr<Val> Eval(Frame* f) const override;
	IntrusivePtr<Val> DoSingleEval(Frame* f, Val* v) const;

	bool IsReduced(Reducer* c) const override;
	bool HasReducedOps(Reducer* c) const override	{ return false; }
	bool WillTransform(Reducer* c) const override	{ return true; }
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
	Expr* ReduceToSingleton(Reducer* c,
				IntrusivePtr<Stmt>& red_stmt) override;
	const CompiledStmt Compile(Compiler* c) const override;

	IntrusivePtr<Expr> Duplicate() override;

	bool IsPure() const override;
	bool HasNoSideEffects() const override;
};

class ComplementExpr : public UnaryExpr {
public:
	explicit ComplementExpr(IntrusivePtr<Expr> op);

protected:
	IntrusivePtr<Val> Fold(Val* v) const override;
	bool WillTransform(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<Expr> Duplicate() override;
};

class NotExpr : public UnaryExpr {
public:
	explicit NotExpr(IntrusivePtr<Expr> op);

protected:
	IntrusivePtr<Val> Fold(Val* v) const override;
	bool WillTransform(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<Expr> Duplicate() override;
};

class PosExpr : public UnaryExpr {
public:
	explicit PosExpr(IntrusivePtr<Expr> op);

protected:
	IntrusivePtr<Val> Fold(Val* v) const override;
	bool WillTransform(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<Expr> Duplicate() override;
};

class NegExpr : public UnaryExpr {
public:
	explicit NegExpr(IntrusivePtr<Expr> op);

protected:
	IntrusivePtr<Val> Fold(Val* v) const override;
	bool WillTransform(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<Expr> Duplicate() override;
};

class SizeExpr : public UnaryExpr {
public:
	explicit SizeExpr(IntrusivePtr<Expr> op);
	IntrusivePtr<Val> Eval(Frame* f) const override;

protected:
	IntrusivePtr<Val> Fold(Val* v) const override;

	IntrusivePtr<Expr> Duplicate() override;
};

class AddExpr : public BinaryExpr {
public:
	AddExpr(IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2);
	void Canonicize() override;

	bool WillTransform(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
	Expr* BuildSub(const IntrusivePtr<Expr>& op1,
			const IntrusivePtr<Expr>& op2);

	IntrusivePtr<Expr> Duplicate() override;
};

class AddToExpr : public BinaryExpr {
public:
	AddToExpr(IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2);
	IntrusivePtr<Val> Eval(Frame* f) const override;

	bool WillTransform(Reducer* c) const override	{ return true; }
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<Expr> Duplicate() override;
};

class AppendToExpr : public BinaryExpr {
public:
	AppendToExpr(IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2);
	IntrusivePtr<Val> Eval(Frame* f) const override;

	bool IsReduced(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
	const CompiledStmt Compile(Compiler* c) const override;

	IntrusivePtr<Expr> Duplicate() override;
};

class RemoveFromExpr : public BinaryExpr {
public:
	RemoveFromExpr(IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2);
	IntrusivePtr<Val> Eval(Frame* f) const override;

	bool WillTransform(Reducer* c) const override	{ return true; }
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<Expr> Duplicate() override;
};

class SubExpr : public BinaryExpr {
public:
	SubExpr(IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2);

	bool WillTransform(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<Expr> Duplicate() override;
};

class TimesExpr : public BinaryExpr {
public:
	TimesExpr(IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2);
	void Canonicize() override;

	bool WillTransform(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<Expr> Duplicate() override;
};

class DivideExpr : public BinaryExpr {
public:
	DivideExpr(IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2);

protected:
	IntrusivePtr<Val> AddrFold(Val* v1, Val* v2) const override;

	bool WillTransform(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<Expr> Duplicate() override;
};

class ModExpr : public BinaryExpr {
public:
	ModExpr(IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2);

	IntrusivePtr<Expr> Duplicate() override;
};

class BoolExpr : public BinaryExpr {
public:
	BoolExpr(BroExprTag tag, IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2);

	IntrusivePtr<Val> Eval(Frame* f) const override;
	IntrusivePtr<Val> DoSingleEval(Frame* f, IntrusivePtr<Val> v1, Expr* op2) const;

	bool WillTransform(Reducer* c) const override	{ return true; }
	bool WillTransformInConditional(Reducer* c) const override;;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<Expr> Duplicate() override;

	bool IsTrue(const IntrusivePtr<Expr>& e) const;
	bool IsFalse(const IntrusivePtr<Expr>& e) const;
};

class BitExpr : public BinaryExpr {
public:
	BitExpr(BroExprTag tag, IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2);

	bool WillTransform(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<Expr> Duplicate() override;
};

class EqExpr : public BinaryExpr {
public:
	EqExpr(BroExprTag tag, IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2);
	void Canonicize() override;

protected:
	IntrusivePtr<Val> Fold(Val* v1, Val* v2) const override;
	bool WillTransform(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<Expr> Duplicate() override;
};

class RelExpr : public BinaryExpr {
public:
	RelExpr(BroExprTag tag, IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2);
	void Canonicize() override;
	bool WillTransform(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<Expr> Duplicate() override;
};

class CondExpr : public Expr {
public:
	CondExpr(IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2, IntrusivePtr<Expr> op3);

	const Expr* Op1() const	{ return op1.get(); }
	const Expr* Op2() const	{ return op2.get(); }
	const Expr* Op3() const	{ return op3.get(); }

	IntrusivePtr<Expr> GetOp1() const override final	{ return op1; }
	IntrusivePtr<Expr> GetOp2() const override final	{ return op2; }
	IntrusivePtr<Expr> GetOp3() const override final	{ return op3; }

	void SetOp1(IntrusivePtr<Expr> _op) override final { op1 = _op; }
	void SetOp2(IntrusivePtr<Expr> _op) override final { op2 = _op; }
	void SetOp3(IntrusivePtr<Expr> _op) override final { op3 = _op; }

	IntrusivePtr<Val> Eval(Frame* f) const override;
	bool IsPure() const override;
	bool IsReduced(Reducer* c) const override;
	bool HasReducedOps(Reducer* c) const override;
	bool WillTransform(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
	IntrusivePtr<Stmt> ReduceToSingletons(Reducer* c) override;

	IntrusivePtr<Expr> Duplicate() override;

	Expr* Inline(Inliner* inl) override;

	TraversalCode Traverse(TraversalCallback* cb) const override;

protected:
	void ExprDescribe(ODesc* d) const override;

	IntrusivePtr<Expr> op1;
	IntrusivePtr<Expr> op2;
	IntrusivePtr<Expr> op3;
};

class RefExpr : public UnaryExpr {
public:
	explicit RefExpr(IntrusivePtr<Expr> op);

	void Assign(Frame* f, IntrusivePtr<Val> v) override;
	IntrusivePtr<Expr> MakeLvalue() override;

	bool IsReduced(Reducer* c) const override;
	bool HasReducedOps(Reducer* c) const override;
	bool WillTransform(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	// Reduce to simplifed LHS form, i.e., a reference to only a name.
	IntrusivePtr<Stmt> ReduceToLHS(Reducer* c);

	IntrusivePtr<Expr> Duplicate() override;
};

class AssignExpr : public BinaryExpr {
public:
	// If val is given, evaluating this expression will always yield the val
	// yet still perform the assignment.  Used for triggers.
	AssignExpr(IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2, bool is_init,
	           IntrusivePtr<Val> val = nullptr, attr_list* attrs = nullptr,
		   bool typecheck = true);

	IntrusivePtr<Val> Eval(Frame* f) const override;
	void EvalIntoAggregate(const BroType* t, Val* aggr, Frame* f) const override;
	IntrusivePtr<BroType> InitType() const override;
	bool IsRecordElement(TypeDecl* td) const override;
	IntrusivePtr<Val> InitVal(const BroType* t, IntrusivePtr<Val> aggr) const override;
	bool IsPure() const override;
	bool HasNoSideEffects() const override;
	bool IsReduced(Reducer* c) const override;
	bool HasReducedOps(Reducer* c) const override;
	bool WillTransform(Reducer* c) const override	{ return true; }
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
	Expr* ReduceToSingleton(Reducer* c,
				IntrusivePtr<Stmt>& red_stmt) override;
	const CompiledStmt Compile(Compiler* c) const override;

	IntrusivePtr<Expr> Duplicate() override;

	// Whether this is an assignment to a temporary.
	bool IsTemp() const	{ return is_temp; }
	void SetIsTemp()	{ is_temp = true; }

protected:
	const CompiledStmt DoCompile(Compiler* c, const NameExpr* lhs) const;
	const CompiledStmt CompileAssignToIndex(Compiler* c,
						const NameExpr* lhs,
						const IndexExpr* rhs) const;

	bool TypeCheck(attr_list* attrs = 0);
	bool TypeCheckArithmetics(TypeTag bt1, TypeTag bt2);

	bool is_init;
	bool is_temp;
	IntrusivePtr<Val> val;	// optional
};

// An internal class for reduced form.
class IndexAssignExpr : public BinaryExpr {
public:
	// "op1[op2] = op3", all reduced.
	IndexAssignExpr(IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2,
			IntrusivePtr<Expr> op3);

	IntrusivePtr<Val> Eval(Frame* f) const override;
	bool IsReduced(Reducer* c) const override;
	bool HasReducedOps(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
	Expr* ReduceToSingleton(Reducer* c,
				IntrusivePtr<Stmt>& red_stmt) override;
	const CompiledStmt Compile(Compiler* c) const override;

	IntrusivePtr<Expr> Duplicate() override;

	IntrusivePtr<Expr> GetOp3() const override final	{ return op3; }
	void SetOp3(IntrusivePtr<Expr> _op) override final { op3 = _op; }

	TraversalCode Traverse(TraversalCallback* cb) const override;

protected:
	void ExprDescribe(ODesc* d) const override;

	IntrusivePtr<Expr> op3;	// assignment RHS
};

class IndexSliceAssignExpr : public AssignExpr {
public:
	IndexSliceAssignExpr(IntrusivePtr<Expr> op1,
	                     IntrusivePtr<Expr> op2, bool is_init);
	IntrusivePtr<Val> Eval(Frame* f) const override;
	const CompiledStmt Compile(Compiler* c) const override;

	IntrusivePtr<Expr> Duplicate() override;
};

class IndexExpr : public BinaryExpr {
public:
	IndexExpr(IntrusivePtr<Expr> op1,
	          IntrusivePtr<ListExpr> op2, bool is_slice = false);

	bool CanAdd() const override;
	bool CanDel() const override;

	void Add(Frame* f) override;
	void Delete(Frame* f) override;

	void Assign(Frame* f, IntrusivePtr<Val> v) override;
	bool HasReducedOps(Reducer* c) const override;
	IntrusivePtr<Expr> MakeLvalue() override;

	// Need to override Eval since it can take a vector arg but does
	// not necessarily return a vector.
	IntrusivePtr<Val> Eval(Frame* f) const override;

	IntrusivePtr<Stmt> ReduceToSingletons(Reducer* c) override;

	IntrusivePtr<Expr> Duplicate() override;

	bool IsSlice() const { return is_slice; }

protected:
	IntrusivePtr<Val> Fold(Val* v1, Val* v2) const override;

	void ExprDescribe(ODesc* d) const override;

	bool is_slice;
};


// Functions used by IndexExpr for evaluation.  Factored out so
// that compiled statements can call them too.

// This first one assumes that a length check has already been made.
extern IntrusivePtr<VectorVal> vector_bool_select(VectorType* t,
						const VectorVal* v1,
						const VectorVal* v2);
extern IntrusivePtr<VectorVal> vector_int_select(VectorType* t,
						const VectorVal* v1,
						const VectorVal* v2);
extern IntrusivePtr<VectorVal> vector_index(VectorType* vt,
						const VectorVal* vect,
						const ListVal* lv);

extern BroString* index_string_slice(const BroString* s, const ListVal* lv);

// Any internal call used for [a, b, c, ...] = x assignments.
class AnyIndexExpr : public UnaryExpr {
public:
	AnyIndexExpr(IntrusivePtr<Expr> op, int index);

	int Index() const	{ return index; }

protected:
	IntrusivePtr<Val> Fold(Val* v) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<Expr> Duplicate() override;

	void ExprDescribe(ODesc* d) const override;

	int index;
};

// An internal class for reduced form.
class FieldLHSAssignExpr : public BinaryExpr {
public:
	// "op1$field = RHS", where RHS is reduced with respect to
	// ReduceToFieldAssignment().
	FieldLHSAssignExpr(IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2,
				const char* field_name, int field);

	const char* FieldName() const	{ return field_name; }
	int Field() const		{ return field; }

	IntrusivePtr<Val> Eval(Frame* f) const override;
	bool IsReduced(Reducer* c) const override;
	bool HasReducedOps(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
	Expr* ReduceToSingleton(Reducer* c,
				IntrusivePtr<Stmt>& red_stmt) override;
	const CompiledStmt Compile(Compiler* c) const override;

	IntrusivePtr<Expr> Duplicate() override;

protected:
	void ExprDescribe(ODesc* d) const override;

	const char* field_name;
	int field;
};

class FieldExpr : public UnaryExpr {
public:
	FieldExpr(IntrusivePtr<Expr> op, const char* field_name);
	~FieldExpr() override;

	int Field() const	{ return field; }
	const char* FieldName() const	{ return field_name; }

	bool CanDel() const override;

	void Assign(Frame* f, IntrusivePtr<Val> v) override;
	void Delete(Frame* f) override;

	IntrusivePtr<Expr> MakeLvalue() override;

protected:
	IntrusivePtr<Val> Fold(Val* v) const override;

	IntrusivePtr<Expr> Duplicate() override;

	void ExprDescribe(ODesc* d) const override;

	const char* field_name;
	const TypeDecl* td;
	int field; // -1 = attributes
};

// "rec?$fieldname" is true if the value of $fieldname in rec is not nil.
// "rec?$$attrname" is true if the attribute attrname is not nil.
class HasFieldExpr : public UnaryExpr {
public:
	HasFieldExpr(IntrusivePtr<Expr> op, const char* field_name);
	~HasFieldExpr() override;

	const char* FieldName() const	{ return field_name; }
	int Field() const		{ return field; }

protected:
	IntrusivePtr<Val> Fold(Val* v) const override;

	IntrusivePtr<Expr> Duplicate() override;

	void ExprDescribe(ODesc* d) const override;

	const char* field_name;
	int field;
};

class RecordConstructorExpr : public UnaryExpr {
public:
	explicit RecordConstructorExpr(IntrusivePtr<ListExpr> constructor_list);

	// This form is used to construct records of a known type.
	explicit RecordConstructorExpr(IntrusivePtr<RecordType> known_rt,
				IntrusivePtr<ListExpr> constructor_list);

	~RecordConstructorExpr() override	{ delete [] map; }

	int* Map() const	{ return map; }

protected:
	IntrusivePtr<Val> InitVal(const BroType* t, IntrusivePtr<Val> aggr) const override;
	IntrusivePtr<Val> Fold(Val* v) const override;

	bool HasReducedOps(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
	IntrusivePtr<Stmt> ReduceToSingletons(Reducer* c) override;

	IntrusivePtr<Expr> Duplicate() override;

	void ExprDescribe(ODesc* d) const override;

	// For constructing records of a known type, map[] provides
	// a mapping from positions in the constructor list to positions
	// in the record we're constructing.
	int* map = nullptr;

	IntrusivePtr<RecordType> rt;	// for convenience
};

class TableConstructorExpr : public UnaryExpr {
public:
	TableConstructorExpr(IntrusivePtr<ListExpr> constructor_list, attr_list* attrs,
	                     IntrusivePtr<BroType> arg_type = nullptr);
	~TableConstructorExpr() override { Unref(attrs); }

	Attributes* Attrs() const	{ return attrs; }

	IntrusivePtr<Val> Eval(Frame* f) const override;

	bool HasReducedOps(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
	IntrusivePtr<Stmt> ReduceToSingletons(Reducer* c) override;

protected:
	IntrusivePtr<Val> InitVal(const BroType* t, IntrusivePtr<Val> aggr) const override;

	IntrusivePtr<Expr> Duplicate() override;

	void ExprDescribe(ODesc* d) const override;

	Attributes* attrs;
};

class SetConstructorExpr : public UnaryExpr {
public:
	SetConstructorExpr(IntrusivePtr<ListExpr> constructor_list, attr_list* attrs,
	                   IntrusivePtr<BroType> arg_type = nullptr);
	~SetConstructorExpr() override { Unref(attrs); }

	Attributes* Attrs() const { return attrs; }

	IntrusivePtr<Val> Eval(Frame* f) const override;

	bool HasReducedOps(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
	IntrusivePtr<Stmt> ReduceToSingletons(Reducer* c) override;

protected:
	IntrusivePtr<Val> InitVal(const BroType* t, IntrusivePtr<Val> aggr) const override;

	IntrusivePtr<Expr> Duplicate() override;

	void ExprDescribe(ODesc* d) const override;

	Attributes* attrs;
};

class VectorConstructorExpr : public UnaryExpr {
public:
	explicit VectorConstructorExpr(IntrusivePtr<ListExpr> constructor_list,
	                               IntrusivePtr<BroType> arg_type = nullptr);

	IntrusivePtr<Val> Eval(Frame* f) const override;
	bool HasReducedOps(Reducer* c) const override;

protected:
	IntrusivePtr<Val> InitVal(const BroType* t, IntrusivePtr<Val> aggr) const override;

	IntrusivePtr<Expr> Duplicate() override;

	void ExprDescribe(ODesc* d) const override;
};

class FieldAssignExpr : public UnaryExpr {
public:
	FieldAssignExpr(const char* field_name, IntrusivePtr<Expr> value);

	const char* FieldName() const	{ return field_name.c_str(); }

	// When these are first constructed, we don't know the type.
	// The following method coerces/promotes the assignment expression
	// as needed, once we do know the type.
	//
	// Returns true on success, false if the types were incompatible
	// (in which case an error is reported).
	bool PromoteTo(BroType* t);

	void EvalIntoAggregate(const BroType* t, Val* aggr, Frame* f) const override;
	bool WillTransform(Reducer* c) const override	{ return true; }
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<Expr> Duplicate() override;

	bool IsRecordElement(TypeDecl* td) const override;

protected:
	void ExprDescribe(ODesc* d) const override;

	string field_name;
};

class ArithCoerceExpr : public UnaryExpr {
public:
	ArithCoerceExpr(IntrusivePtr<Expr> op, TypeTag t);

protected:
	IntrusivePtr<Val> FoldSingleVal(IntrusivePtr<Val> v) const;
	IntrusivePtr<Val> Fold(Val* v) const override;

	IntrusivePtr<Expr> Duplicate() override;

	bool WillTransform(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
};

class RecordCoerceExpr : public UnaryExpr {
public:
	RecordCoerceExpr(IntrusivePtr<Expr> op, IntrusivePtr<RecordType> r);
	~RecordCoerceExpr() override;

	// These are only made available for the compiler.  Alternatively,
	// we could make it a friend class.
	int* Map() const	{ return map; }
	int MapSize() const	{ return map_size; }

protected:
	IntrusivePtr<Val> InitVal(const BroType* t, IntrusivePtr<Val> aggr) const override;
	IntrusivePtr<Val> Fold(Val* v) const override;

	IntrusivePtr<Expr> Duplicate() override;

	// For each super-record slot, gives subrecord slot with which to
	// fill it.
	int* map;
	int map_size;	// equivalent to Type()->AsRecordType()->NumFields()
};

extern IntrusivePtr<RecordVal> coerce_to_record(RecordType* rt, Val* v,
						int* map, int map_size);

class TableCoerceExpr : public UnaryExpr {
public:
	TableCoerceExpr(IntrusivePtr<Expr> op, IntrusivePtr<TableType> r);

protected:
	IntrusivePtr<Val> Fold(Val* v) const override;

	IntrusivePtr<Expr> Duplicate() override;
};

class VectorCoerceExpr : public UnaryExpr {
public:
	VectorCoerceExpr(IntrusivePtr<Expr> op, IntrusivePtr<VectorType> v);

protected:
	IntrusivePtr<Val> Fold(Val* v) const override;

	IntrusivePtr<Expr> Duplicate() override;
};

class CoerceToAnyExpr : public UnaryExpr {
public:
	CoerceToAnyExpr(IntrusivePtr<Expr> op);

protected:
	IntrusivePtr<Val> Fold(Val* v) const override;

	IntrusivePtr<Expr> Duplicate() override;
};

class CoerceFromAnyExpr : public UnaryExpr {
public:
	CoerceFromAnyExpr(IntrusivePtr<Expr> op, IntrusivePtr<BroType> to_type);

protected:
	IntrusivePtr<Val> Fold(Val* v) const override;

	IntrusivePtr<Expr> Duplicate() override;
};

class ScheduleTimer : public Timer {
public:
	ScheduleTimer(const EventHandlerPtr& event, zeek::Args args, double t);
	~ScheduleTimer() override;

	void Dispatch(double t, bool is_expire) override;

protected:
	EventHandlerPtr event;
	zeek::Args args;
};

class ScheduleExpr : public Expr {
public:
	ScheduleExpr(IntrusivePtr<Expr> when, IntrusivePtr<EventExpr> event);

	bool IsPure() const override;
	bool IsReduced(Reducer* c) const override;
	bool HasReducedOps(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
	Expr* Inline(Inliner* inl) override;
	const CompiledStmt Compile(Compiler* c) const override;

	IntrusivePtr<Expr> Duplicate() override;

	IntrusivePtr<Val> Eval(Frame* f) const override;

	Expr* When() const	{ return when.get(); }
	EventExpr* Event() const	{ return event.get(); }

	IntrusivePtr<Expr> GetOp1() const override final;
	IntrusivePtr<Expr> GetOp2() const override final;

	void SetOp1(IntrusivePtr<Expr> _op) override final;
	void SetOp2(IntrusivePtr<Expr> _op) override final;

	TraversalCode Traverse(TraversalCallback* cb) const override;

protected:
	void ExprDescribe(ODesc* d) const override;

	IntrusivePtr<Expr> when;
	IntrusivePtr<EventExpr> event;
};

class InExpr : public BinaryExpr {
public:
	InExpr(IntrusivePtr<Expr> op1, IntrusivePtr<Expr> op2);

	bool HasReducedOps(Reducer* c) const override;

protected:
	IntrusivePtr<Val> Fold(Val* v1, Val* v2) const override;

	IntrusivePtr<Expr> Duplicate() override;

};

class CallExpr : public Expr {
public:
	CallExpr(IntrusivePtr<Expr> func, IntrusivePtr<ListExpr> args,
	         bool in_hook = false);

	Expr* Func() const	{ return func.get(); }
	ListExpr* Args() const	{ return args.get(); }

	bool IsPure() const override;
	bool IsReduced(Reducer* c) const override;
	bool HasReducedOps(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
	IntrusivePtr<Stmt> ReduceToSingletons(Reducer* c) override;
	Expr* Inline(Inliner* inl) override;
	const CompiledStmt Compile(Compiler* c) const override;

	IntrusivePtr<Val> Eval(Frame* f) const override;

	IntrusivePtr<Expr> Duplicate() override;

	TraversalCode Traverse(TraversalCallback* cb) const override;

protected:
	void ExprDescribe(ODesc* d) const override;

	IntrusivePtr<Expr> func;
	IntrusivePtr<ListExpr> args;

	// The following is just for optimizing calls, to ensure we
	// don't spend time hunting for run-time "any" argument mismatches
	// if there's no possibility of encountering one.
	bool has_any_arg;
};

class InlineExpr : public Expr {
public:
	InlineExpr(IntrusivePtr<ListExpr> arg_args, id_list* params,
			IntrusivePtr<Stmt> body, int frame_offset,
			IntrusivePtr<BroType> ret_type);

	bool IsPure() const override;
	bool IsReduced(Reducer* c) const override;
	bool HasReducedOps(Reducer* c) const override	{ return false; }
	bool WillTransform(Reducer* c) const override	{ return true; }
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	IntrusivePtr<ListExpr> Args() const	{ return args; }
	IntrusivePtr<Stmt> Body() const		{ return body; }

	IntrusivePtr<Val> Eval(Frame* f) const override;

	IntrusivePtr<Expr> Duplicate() override;

	TraversalCode Traverse(TraversalCallback* cb) const override;

protected:
	void ExprDescribe(ODesc* d) const override;

	id_list* params;
	int frame_offset;
	IntrusivePtr<ListExpr> args;
	IntrusivePtr<Stmt> body;
};


/**
 * Class that represents an anonymous function expression in Zeek.
 * On evaluation, captures the frame that it is evaluated in. This becomes
 * the closure for the instance of the function that it creates.
 */
class LambdaExpr : public Expr {
public:
	LambdaExpr(std::unique_ptr<function_ingredients> ingredients,
		   id_list outer_ids);

	IntrusivePtr<Val> Eval(Frame* f) const override;

	Expr* Inline(Inliner* inl) override;
	IntrusivePtr<Expr> Duplicate() override;

	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;

	const CompiledStmt Compile(Compiler* c) const override;

	id_list OuterIDs() const	{ return outer_ids; }

	TraversalCode Traverse(TraversalCallback* cb) const override;

	Scope* GetScope() const;

protected:
	void ExprDescribe(ODesc* d) const override;

private:
	IntrusivePtr<BroFunc> master_func;
	std::unique_ptr<function_ingredients> ingredients;

	id_list outer_ids;
	std::string my_name;
};

class ListExpr : public Expr {
public:
	ListExpr();
	explicit ListExpr(IntrusivePtr<Expr> e);
	~ListExpr() override;

	void Append(IntrusivePtr<Expr> e);

	const expr_list& Exprs() const	{ return exprs; }
	expr_list& Exprs()		{ return exprs; }

	// True if the entire list represents pure values / reduced expressions.
	bool IsPure() const override;
	bool IsReduced(Reducer* c) const override;
	bool HasReducedOps(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
	IntrusivePtr<Stmt> ReduceToSingletons(Reducer* c) override;
	Expr* Inline(Inliner* inl) override;

	IntrusivePtr<Val> Eval(Frame* f) const override;

	IntrusivePtr<Expr> Duplicate() override;

	IntrusivePtr<BroType> InitType() const override;
	IntrusivePtr<Val> InitVal(const BroType* t, IntrusivePtr<Val> aggr) const override;
	IntrusivePtr<Expr> MakeLvalue() override;
	void Assign(Frame* f, IntrusivePtr<Val> v) override;

	TraversalCode Traverse(TraversalCallback* cb) const override;

protected:
	IntrusivePtr<Val> AddSetInit(const BroType* t, IntrusivePtr<Val> aggr) const;

	void ExprDescribe(ODesc* d) const override;

	expr_list exprs;
};

class EventExpr : public Expr {
public:
	EventExpr(const char* name, IntrusivePtr<ListExpr> args);

	const char* Name() const	{ return name.c_str(); }
	ListExpr* Args() const		{ return args.get(); }
	EventHandlerPtr Handler()  const	{ return handler; }

	IntrusivePtr<Val> Eval(Frame* f) const override;

	bool IsPure() const override	{ return false; }
	bool IsReduced(Reducer* c) const override;
	Expr* Reduce(Reducer* c, IntrusivePtr<Stmt>& red_stmt) override;
	IntrusivePtr<Stmt> ReduceToSingletons(Reducer* c) override;
	Expr* Inline(Inliner* inl) override;
	const CompiledStmt Compile(Compiler* c) const override;
	IntrusivePtr<Expr> Duplicate() override;

	IntrusivePtr<Expr> GetOp1() const override final	{ return args; }
	void SetOp1(IntrusivePtr<Expr> _op) override final
		{ args = {NewRef{}, _op->AsListExpr()}; }

	TraversalCode Traverse(TraversalCallback* cb) const override;

protected:
	void ExprDescribe(ODesc* d) const override;

	string name;
	EventHandlerPtr handler;
	IntrusivePtr<ListExpr> args;
};


class RecordAssignExpr : public ListExpr {
public:
	RecordAssignExpr(const IntrusivePtr<Expr>& record, const IntrusivePtr<Expr>& init_list, bool is_init);
};

class CastExpr : public UnaryExpr {
public:
	CastExpr(IntrusivePtr<Expr> op, IntrusivePtr<BroType> t);

protected:
	IntrusivePtr<Val> Eval(Frame* f) const override;
	IntrusivePtr<Expr> Duplicate() override;
	void ExprDescribe(ODesc* d) const override;
};

extern IntrusivePtr<Val> cast_value(Val* v, BroType* t, const char*& error);

class IsExpr : public UnaryExpr {
public:
	IsExpr(IntrusivePtr<Expr> op, IntrusivePtr<BroType> t);

	const IntrusivePtr<BroType>& TestType() const	{ return t; }

protected:
	IntrusivePtr<Val> Fold(Val* v) const override;
	IntrusivePtr<Expr> Duplicate() override;
	void ExprDescribe(ODesc* d) const override;

private:
	IntrusivePtr<BroType> t;
};


// Used internally for optimization.
class NopExpr : public Expr {
public:
	explicit NopExpr() : Expr(EXPR_NOP) { }

	IntrusivePtr<Val> Eval(Frame* f) const override;

	TraversalCode Traverse(TraversalCallback* cb) const override;

protected:
	void ExprDescribe(ODesc* d) const override;
};

inline Val* Expr::ExprVal() const
	{
	if ( ! IsConst() )
		BadTag("ExprVal::Val", expr_name(tag), expr_name(EXPR_CONST));
	return ((ConstExpr*) this)->Value();
	}

// Decides whether to return an AssignExpr or a RecordAssignExpr.
IntrusivePtr<Expr> get_assign_expr(IntrusivePtr<Expr> op1,
                                   IntrusivePtr<Expr> op2, bool is_init);

// Helper function for making an assignment to an LHS that's
// a temporary.
IntrusivePtr<Expr> get_temp_assign_expr(IntrusivePtr<Expr> op1,
					   IntrusivePtr<Expr> op2);

// Type-check the given expression(s) against the given type(s).  Complain
// if the expression cannot match the given type, returning nullptr;
// otherwise, returns an expression reflecting the promotion.
//
// The second, third, and fourth forms are for promoting a list of
// expressions (which is updated in place) to either match a list of
// types or a single type.
//
// Note, the type is not "const" because it can be ref'd.

/**
 * Returns nullptr if the expression cannot match or a promoted
 * expression.
 */
extern IntrusivePtr<Expr> check_and_promote_expr(Expr* e, BroType* t);

extern bool check_and_promote_exprs(ListExpr* elements, TypeList* types);
extern bool check_and_promote_args(ListExpr* args, RecordType* types);
extern bool check_and_promote_exprs_to_type(ListExpr* elements, BroType* type);

// Returns a ListExpr simplified down to a list a values, or nil
// if they couldn't all be reduced.
std::optional<std::vector<IntrusivePtr<Val>>> eval_list(Frame* f, const ListExpr* l);

// Returns true if e1 is "greater" than e2 - here "greater" is just
// a heuristic, used with commutative operators to put them into
// a canonical form.
extern bool expr_greater(const Expr* e1, const Expr* e2);

// Returns true if e1 and e2 are both singletons and further they represent
// equivalent singletons.
extern bool same_singletons(IntrusivePtr<Expr> e1, IntrusivePtr<Expr> e2);

// True if the given Val* has a vector type
inline bool is_vector(Expr* e)	{ return e->Type()->Tag() == TYPE_VECTOR; }
