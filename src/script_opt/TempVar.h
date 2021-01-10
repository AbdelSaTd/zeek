// See the file "COPYING" in the main distribution directory for copyright.

#pragma once

// Class for managing temporary variables created during statement reduction
// for compilation.

#include "ID.h"
#include "Expr.h"


namespace zeek::detail {

class TempVar {
public:
	TempVar(int num, const TypePtr& t, ExprPtr rhs);
	~TempVar()	{ delete name; }

	const char* Name() const	{ return name; }
	const zeek::Type* Type() const	{ return type.get(); }
	const Expr* RHS() const		{ return rhs.get(); }

	IDPtr Id() const	{ return id; }
	void SetID(IDPtr _id)	{ id = std::move(_id); }
	void Deactivate()	{ active = false; }
	bool IsActive() const	{ return active; }

protected:
	char* name;
	IDPtr id;
	const TypePtr& type;
	ExprPtr rhs;
	bool active = true;
};

} // zeek::detail
