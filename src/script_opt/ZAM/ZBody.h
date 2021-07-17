// See the file "COPYING" in the main distribution directory for copyright.

// ZBody: ZAM function body that replaces a function's original AST body.

#pragma once

#include "zeek/script_opt/ZAM/Support.h"
#include "zeek/script_opt/ZAM/ZInst.h"

namespace zeek::detail {

// Static information about globals used in a function.  There's a parallel
// array "global_state" that's constructed per-function-invocation that
// dynamically tracks whether a global is loaded, clean, or dirty.
class GlobalInfo {
public:
	IDPtr id;
	int slot;
};

class ZBody : public Stmt {
public:
	// These are the counterparts to CaseMapI and CaseMapsI in ZAM.h,
	// but now concretized to use instruction numbers rather than pointers
	// to instructions.
	template<typename T> using CaseMap = std::map<T, int>;
	template<typename T> using CaseMaps = std::vector<CaseMap<T>>;

	ZBody(const char* _func_name, FrameReMap& _frame_denizens,
	      std::vector<int>& _managed_slots,
	      std::vector<GlobalInfo>& _globals,
	      int num_iters, bool non_recursive,
	      CaseMaps<bro_int_t>& _int_cases,
	      CaseMaps<bro_uint_t>& _uint_cases,
	      CaseMaps<double>& _double_cases,
	      CaseMaps<std::string>& _str_cases);

	~ZBody() override;

	// These are split out from the constructor to allow construction
	// of a ZBody from either save-file full instructions (first method)
	// or intermediary instructions (second method).
	void SetInsts(std::vector<ZInst*>& insts);
	void SetInsts(std::vector<ZInstI*>& instsI);

	ValPtr Exec(Frame* f, StmtFlowType& flow) override;

	// Older code exists for save files, but let's see if we can
	// avoid having to support them, as they're a fairly elaborate
	// production.
	//
	// void SaveTo(FILE* f, int interp_frame_size) const;

	void Dump() const;

	void ProfileExecution() const;

protected:
	friend class ZAMResumption;

	// Initializes profiling information, if needed.
	void InitProfile();

	ValPtr DoExec(Frame* f, int start_pc, StmtFlowType& flow);

	// Run-time checking for "any" type being consistent with
	// expected typed.  Returns true if the type match is okay.
	bool CheckAnyType(const TypePtr& any_type, const TypePtr& expected_type,
	                  const Location* loc) const;

	StmtPtr Duplicate() override	{ return {NewRef{}, this}; }

	void StmtDescribe(ODesc* d) const override;
	TraversalCode Traverse(TraversalCallback* cb) const override;

private:
	const char* func_name;

	const ZInst* insts = nullptr;
	unsigned int ninst;

	FrameReMap frame_denizens;
	int frame_size;

	// A list of frame slots that correspond to managed values.
	std::vector<int> managed_slots;

	// Number of iteration loops, for recursive functions.
	int num_iters;

	// This is non-nil if the function is (asserted to be) non-recursive,
	// in which case we pre-allocate this.
	ZVal* fixed_frame = nullptr;

	std::vector<GlobalInfo> globals;
	int num_globals;

	// The following are only maintained if we're doing profiling.
	//
	// These need to be pointers so we can manipulate them in a
	// const method.
	std::vector<int>* inst_count = nullptr;	// for profiling
	double* CPU_time = nullptr;	// cumulative CPU time for the program
	std::vector<double>* inst_CPU;	// per-instruction CPU time.

	CaseMaps<bro_int_t> int_cases;
	CaseMaps<bro_uint_t> uint_cases;
	CaseMaps<double> double_cases;
	CaseMaps<std::string> str_cases;
};

// This is a statement that resumes execution into a code block in a
// ZBody.  Used for deferred execution for "when" statements.

class ZAMResumption : public Stmt {
public:
	ZAMResumption(ZBody* _am, int _xfer_pc)
	: Stmt(STMT_ZAM_RESUMPTION)
		{
		am = _am;
		xfer_pc = _xfer_pc;
		}

	ValPtr Exec(Frame* f, StmtFlowType& flow) override;

	StmtPtr Duplicate() override	{ return {NewRef{}, this}; }

	void StmtDescribe(ODesc* d) const override;

protected:
	TraversalCode Traverse(TraversalCallback* cb) const override;

	ZBody* am;
	int xfer_pc = 0;
};


// Prints the execution profile.
extern void report_ZOP_profile();

} // namespace zeek::detail
