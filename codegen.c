/*    compile.c
 *
 *    Copyright (C) 2009 by Larry Wall and others
 *
 *    You may distribute under the terms of either the GNU General Public
 *    License or the Artistic License, as specified in the README file.
 *
 */

/*
=head2 Code-generation

The optree is translated into code. 
C<compile_op> add the translation of C<o> branch to the codesequence.
Which is used recursively to translate the whole optree.
The default translation is to translate each op to the corresponding
instruction using postfix order. If a the optype has a OA_MARK then
before the children a "pp_pushmark" instruction is added.
This is the default which is fine for ops which just operating on
their arguments.
Of course this doesn't work for ops like conditionals and loops, these
ops have their own code generation in C<compile_op>. 

During code-generation the codeseq generated may be realloc so, no
pointers to it can be made.
Also the optree may be shared between threads and may not be modified
in any way.

=head3 Constant folding

C<compile_op> has the C<may_constant_fold> argument which should be
set to false if the instructions added to the codesequence may not be
constant folded.

If a op may be constant folded and non of its children sets
C<may_constant_fold> to false, the sequence of instruction can is
converted by executing the instructions for this op and executing
them, and replacing the instruction which a C<instr_const> instruction
with the returned C<SV>.
To save a instruction pointer to a C<pparg1> the
C<save_instr_from_to_pparg> should be used.

To handle special cases if there is a constant (or constant folded
op), C<svp_const_instruction> can be used to retrieve the value of the
constant of the last instruction (which should be constant or constant
folded).

=head3 Jump targets

Jumping is done by setting the "next instruction pointer", to save the
instruction C<save_branch_point> which saves the translation point
into the address specified. Note that the during translation the
addresses of the instruction are not yet fixed (they might be
C<realloced>), so the addresses actually writing of the intruction
address to the specified address happens at the end of the
code-generation.

=head3 Instruction arguments

Because the optree can't be modified during code-generation, arguments
can be added to the instruction, these have the C<void*> type by
default so they should normally be typecasted.

=head3 Debugging

If perl is compiled with C<-DDEBUGGING> the command line options
C<-DG> and C<-Dg> can be used. The C<-DG> option will dump the result
of the code generation after it is finished (note that the labels in
this dump are generation by the dump and only pointers to the
instruction are present in the actual code). The C<-Dg> option will
trace the code generation process.

=cut
*/

#include "EXTERN.h"
#define PERL_IN_COMPILE_C
#include "perl.h"

struct op_instrpp {
    INSTRUCTION** instrpp;
    int instr_idx;
};
typedef struct op_instrpp OP_INSTRPP;

struct branch_point_to_pparg {
    int instr_from_index;
    int instr_to_index;
};
typedef struct branch_point_to_pparg BRANCH_POINT_TO_PPARG;

struct codegen_pad {
    CODESEQ codeseq;
    int idx;
    OP_INSTRPP* op_instrpp_list;
    OP_INSTRPP* op_instrpp_end;
    OP_INSTRPP* op_instrpp_append;
    BRANCH_POINT_TO_PPARG* branch_point_to_pparg_list;
    BRANCH_POINT_TO_PPARG* branch_point_to_pparg_end;
    BRANCH_POINT_TO_PPARG* branch_point_to_pparg_append;
    int recursion;
};

void
S_append_instruction_x(pTHX_ CODEGEN_PAD* bpp, OP* o,
    Optype optype, void* instr_arg1, void* instr_arg2)
{
    PERL_ARGS_ASSERT_APPEND_INSTRUCTION_X;
    bpp->codeseq.xcodeseq_instructions[bpp->idx].instr_ppaddr = PL_ppaddr[optype];
    bpp->codeseq.xcodeseq_instructions[bpp->idx].instr_op = o;
    bpp->codeseq.xcodeseq_instructions[bpp->idx].instr_arg1 = instr_arg1;
    bpp->codeseq.xcodeseq_instructions[bpp->idx].instr_arg2 = instr_arg2;

    bpp->idx++;
    if (bpp->idx >= bpp->codeseq.xcodeseq_size) {
	bpp->codeseq.xcodeseq_size += 32;
	Renew(bpp->codeseq.xcodeseq_instructions, bpp->codeseq.xcodeseq_size, INSTRUCTION);
    }

}

void
S_append_instruction(pTHX_ CODEGEN_PAD* bpp, OP* o, Optype optype)
{
    PERL_ARGS_ASSERT_APPEND_INSTRUCTION;
    append_instruction_x(bpp, o, optype, NULL, NULL);
}

void
S_save_branch_point(pTHX_ CODEGEN_PAD* bpp, INSTRUCTION** instrp)
{
    PERL_ARGS_ASSERT_SAVE_BRANCH_POINT;
    DEBUG_g(Perl_deb(aTHX_ "registering branch point "); Perl_deb(aTHX_ "\n"));
    if (bpp->op_instrpp_append >= bpp->op_instrpp_end) {
	OP_INSTRPP* old_lp = bpp->op_instrpp_list;
	int new_size = 128 + (bpp->op_instrpp_end - bpp->op_instrpp_list);
	Renew(bpp->op_instrpp_list, new_size, OP_INSTRPP);
	bpp->op_instrpp_end = bpp->op_instrpp_list + new_size;
	bpp->op_instrpp_append = bpp->op_instrpp_list + (bpp->op_instrpp_append - old_lp);
    }
    assert(bpp->op_instrpp_append < bpp->op_instrpp_end);
    bpp->op_instrpp_append->instrpp = instrp;
    bpp->op_instrpp_append->instr_idx = bpp->idx;
    bpp->op_instrpp_append++;
}

/* Saves the instruction index difference to the pparg of the "instr_from_index" added instruction */
void
S_save_instr_from_to_pparg(pTHX_ CODEGEN_PAD* codegen_pad, int instr_from_index, int instr_to_index)
{
    PERL_ARGS_ASSERT_SAVE_INSTR_FROM_TO_PPARG;
    if (codegen_pad->branch_point_to_pparg_append >= codegen_pad->branch_point_to_pparg_end) {
	BRANCH_POINT_TO_PPARG* old_lp = codegen_pad->branch_point_to_pparg_list;
	int new_size = 128 + (codegen_pad->branch_point_to_pparg_end - codegen_pad->branch_point_to_pparg_list);
	Renew(codegen_pad->branch_point_to_pparg_list, new_size, BRANCH_POINT_TO_PPARG);
	codegen_pad->branch_point_to_pparg_end = codegen_pad->branch_point_to_pparg_list + new_size;
	codegen_pad->branch_point_to_pparg_append = codegen_pad->branch_point_to_pparg_list + (codegen_pad->branch_point_to_pparg_append - old_lp);
    }
    assert(codegen_pad->branch_point_to_pparg_append < codegen_pad->branch_point_to_pparg_end);
    codegen_pad->branch_point_to_pparg_append->instr_from_index = instr_from_index;
    codegen_pad->branch_point_to_pparg_append->instr_to_index = instr_to_index;
    codegen_pad->branch_point_to_pparg_append++;
}

/* executes the instruction given to it, and returns the SV pushed on the stack by it.
   if C<list> is true, items added to the stack are returned as an AV.
   NULL is returned if an error occured during execution.
   The caller is responsible for decrementing the reference count of the returned SV.
 */
SV*
S_instr_fold_constants(pTHX_ INSTRUCTION* instr, OP* o, bool list)
{
    dVAR;
    SV * VOL sv = NULL;
    int ret = 0;
    I32 oldscope;
    SV * const oldwarnhook = PL_warnhook;
    SV * const olddiehook  = PL_diehook;
    const INSTRUCTION* VOL old_next_instruction = run_get_next_instruction();
    I32 oldsp = PL_stack_sp - PL_stack_base;
    dJMPENV;

    PERL_ARGS_ASSERT_INSTR_FOLD_CONSTANTS;
    DEBUG_g( Perl_deb(aTHX_ "Constant folding "); dump_op_short(o); PerlIO_printf(Perl_debug_log, "\n") );

    oldscope = PL_scopestack_ix;

    PL_op = o;
    create_eval_scope(G_FAKINGEVAL);

    PL_warnhook = PERL_WARNHOOK_FATAL;
    PL_diehook  = NULL;
    JMPENV_PUSH(ret);

    switch (ret) {
    case 0:
	if (list) {
	    PUSHMARK(PL_stack_sp);
	}
	RUN_SET_NEXT_INSTRUCTION(instr);
	CALLRUNOPS(aTHX);
	if (list) {
	    SV** spi;
	    AV* av = newAV();
	    for (spi = PL_stack_base + oldsp + 1; spi <= PL_stack_sp; spi++)
		av_push(av, newSVsv(*spi));
	    PL_stack_sp = PL_stack_base + oldsp;
	    sv = MUTABLE_SV(av);
	}
	else {
	    if (PL_stack_sp - 1 == PL_stack_base + oldsp) {
		sv = *(PL_stack_sp--);
		if (o->op_targ && sv == PAD_SV(o->op_targ)) {	/* grab pad temp? */
		    pad_swipe(o->op_targ,  FALSE);
		}
		else if (SvTEMP(sv)) {			/* grab mortal temp? */
		    SvREFCNT_inc_simple_void(sv);
		    SvTEMP_off(sv);
		}
		else {
		    SvREFCNT_inc_simple_void(sv);       /* immortal ? */
		}
	    }
	}
	break;
    case 3:
	/* Something tried to die.  Abandon constant folding.  */
	/* Pretend the error never happened.  */
	CLEAR_ERRSV();
	break;
    default:
	JMPENV_POP;
	/* Don't expect 1 (setjmp failed) or 2 (something called my_exit)  */
	PL_warnhook = oldwarnhook;
	PL_diehook  = olddiehook;
	/* XXX note that this croak may fail as we've already blown away
	 * the stack - eg any nested evals */
	Perl_croak(aTHX_ "panic: fold_constants JMPENV_PUSH returned %d", ret);
    }
    JMPENV_POP;
    PL_warnhook = oldwarnhook;
    PL_diehook  = olddiehook;
    if (PL_scopestack_ix > oldscope)
	delete_eval_scope();
    assert(PL_scopestack_ix == oldscope);
    RUN_SET_NEXT_INSTRUCTION(old_next_instruction); 

    return sv;
}

void
S_add_kids(pTHX_ CODEGEN_PAD* bpp, OP* o, bool *may_constant_fold)
{
    PERL_ARGS_ASSERT_ADD_KIDS;
    if (o->op_flags & OPf_KIDS) {
	OP* kid;
	for (kid=cUNOPo->op_first; kid; kid=kid->op_sibling)
	    add_op(bpp, kid, may_constant_fold, 0);
    }
}

#define ADDOPf_BOOLEANCONTEXT  1

void
S_add_op(pTHX_ CODEGEN_PAD* bpp, OP* o, bool *may_constant_fold, int flags)
{
    bool kid_may_constant_fold = TRUE;
    int start_idx = bpp->idx;
    bool boolean_context = (flags & ADDOPf_BOOLEANCONTEXT) != 0;

    PERL_ARGS_ASSERT_ADD_OP;

    bpp->recursion++;
    DEBUG_g(
	Perl_deb(aTHX_ "%*sCompiling op sequence ", 2*bpp->recursion, "");
	dump_op_short(o);
	    PerlIO_printf(Perl_debug_log, "\n") );
    
    assert(o);

    switch (o->op_type) {
    case OP_GREPSTART:
    case OP_MAPSTART: {
	/*
	      ...
	      pushmark
	      <o->op_start>
	      grepstart         label2
	  label1:
	      <o->op_more_op>
	      grepwhile         label1
	  label2:
	      ...
	*/
	bool is_grep = o->op_type == OP_GREPSTART;
	int grepstart_idx, grepitem_idx;
	OP* op_block;
	OP* kid;

	op_block = cLISTOPo->op_first;
	assert(op_block->op_type == OP_NULL);

	append_instruction(bpp, NULL, OP_PUSHMARK);
	for (kid=op_block->op_sibling; kid; kid=kid->op_sibling)
	    add_op(bpp, kid, &kid_may_constant_fold, 0);
	append_instruction(bpp, o, o->op_type);

	grepstart_idx = bpp->idx-1;

	grepitem_idx = bpp->idx;
	add_op(bpp, cUNOPx(op_block)->op_first, &kid_may_constant_fold, 0);

	append_instruction(bpp, o, is_grep ? OP_GREPWHILE : OP_MAPWHILE );
	save_instr_from_to_pparg(bpp, bpp->idx-1, grepitem_idx);

	save_instr_from_to_pparg(bpp, grepstart_idx, bpp->idx);

	break;
    }
    case OP_COND_EXPR: {
	/*
	      ...
	      <op_first>
	      cond_expr                label1
	      <op_true>
	      instr_jump               label2
	  label1:
	      <op_false>
	  label2:
	      ...
	*/
	int jump_idx;
	OP* op_first = cLOGOPo->op_first;
	OP* op_true = op_first->op_sibling;
	OP* op_false = op_true->op_sibling;
	bool cond_may_constant_fold = TRUE;

	add_op(bpp, op_first, &cond_may_constant_fold, 0);

	if (cond_may_constant_fold) {
	    SV* const constsv = *(svp_const_instruction(bpp, bpp->idx-1));
	    bpp->idx--;
	    add_op(bpp, SvTRUE(constsv) ? op_true : op_false , &kid_may_constant_fold, 0);
	    break;
	}

	append_instruction(bpp, o, o->op_type);

	/* true branch */
	add_op(bpp, op_true, &kid_may_constant_fold, 0);

	jump_idx = bpp->idx;
	append_instruction_x(bpp, NULL, OP_INSTR_JUMP, NULL, NULL);

	/* false branch */
	save_branch_point(bpp, &(cLOGOPo->op_other_instr));
	add_op(bpp, op_false, &kid_may_constant_fold, 0);

	save_instr_from_to_pparg(bpp, jump_idx, bpp->idx);

	break;
    }
    case OP_ENTERLOOP: {
	/*
	      ...
	      enterloop         last=label3 redo=label4 next=label5
	  label1:
	      <op_start>
	      instr_cond_jump   label2
	  label4:
	      <op_block>
	  label5:
	      <op_cont>
	      instr_jump        label1
	  label2:
	      leaveloop
	  label3:
	      ...
	*/
	int start_idx;
	int cond_jump_idx;
	OP* op_start = cLOOPo->op_first;
	OP* op_block = op_start->op_sibling;
	OP* op_cont = op_block->op_sibling;
	bool has_condition = op_start->op_type != OP_NOTHING;
	append_instruction(bpp, o, o->op_type);

	/* evaluate condition */
	start_idx = bpp->idx;
	if (has_condition) {
	    add_op(bpp, op_start, &kid_may_constant_fold, 0);
	    cond_jump_idx = bpp->idx;
	    append_instruction_x(bpp, NULL, OP_INSTR_COND_JUMP, NULL, NULL);
	}

	save_branch_point(bpp, &(cLOOPo->op_redo_instr));
	add_op(bpp, op_block, &kid_may_constant_fold, 0);

	save_branch_point(bpp, &(cLOOPo->op_next_instr));
	if (op_cont)
	    add_op(bpp, op_cont, &kid_may_constant_fold, 0);

	/* loop */
	if (has_condition) {
	    append_instruction_x(bpp, NULL, OP_INSTR_JUMP, NULL, NULL);
	    save_instr_from_to_pparg(bpp, bpp->idx-1, start_idx);

	    save_instr_from_to_pparg(bpp, cond_jump_idx, bpp->idx);
	}
		
	append_instruction(bpp, o, OP_LEAVELOOP);

	save_branch_point(bpp, &(cLOOPo->op_last_instr));
	break;
    }
    case OP_FOREACH: {
	/*
	      ...
	      pp_pushmark
	      <op_expr>
	      <op_sv>
	      enteriter         redo=label_redo  next=label_next  last=label_last
	  label_start:
	      iter
	      and               label_leave
	  label_redo:
	      <op_block>
	  label_next:
	      unstack
	      <op_cont>
	      instr_jump        label_start
	  label_leave:
	      leaveloop
	  label_last:
	      ...
	*/
	int start_idx;
	int cond_jump_idx;
	OP* op_expr = cLOOPo->op_first;
	OP* op_sv = op_expr->op_sibling;
	OP* op_block = op_sv->op_sibling;
	OP* op_cont = op_block->op_sibling;

	append_instruction(bpp, NULL, OP_PUSHMARK);
	{
	    if (op_expr->op_type == OP_RANGE) {
		/* Basically turn for($x..$y) into the same as for($x,$y), but we
		 * set the STACKED flag to indicate that these values are to be
		 * treated as min/max values by 'pp_iterinit'.
		 */
		LOGOP* const range = (LOGOP*)op_expr;
		UNOP* const flip = cUNOPx(range->op_first);
		add_op(bpp, flip->op_first, &kid_may_constant_fold, 0);
		add_op(bpp, flip->op_first->op_sibling, &kid_may_constant_fold, 0);
	    }
	    else if (op_expr->op_type == OP_REVERSE) {
		add_kids(bpp, op_expr, &kid_may_constant_fold);
	    }
	    else {
		add_op(bpp, op_expr, &kid_may_constant_fold, 0);
	    }
	    if (op_sv->op_type != OP_NOTHING)
		add_op(bpp, op_sv, &kid_may_constant_fold, 0);
	}
	append_instruction(bpp, o, OP_ENTERITER);

	start_idx = bpp->idx;
	append_instruction(bpp, o, OP_ITER);

	cond_jump_idx = bpp->idx;
	append_instruction_x(bpp, NULL, OP_INSTR_COND_JUMP, NULL, NULL);

	save_branch_point(bpp, &(cLOOPo->op_redo_instr));
	add_op(bpp, op_block, &kid_may_constant_fold, 0);

	save_branch_point(bpp, &(cLOOPo->op_next_instr));
	append_instruction_x(bpp, NULL, OP_UNSTACK, NULL, NULL);
	if (op_cont)
	    add_op(bpp, op_cont, &kid_may_constant_fold, 0);

	/* loop */
	append_instruction_x(bpp, NULL, OP_INSTR_JUMP, NULL, NULL);
	save_instr_from_to_pparg(bpp, bpp->idx-1, start_idx);

	save_instr_from_to_pparg(bpp, cond_jump_idx, bpp->idx);
	append_instruction_x(bpp, NULL, OP_LEAVELOOP, NULL, NULL);

	save_branch_point(bpp, &(cLOOPo->op_last_instr));
		
	break;
    }
    case OP_WHILE_AND: {
	OP* op_first = cLOGOPo->op_first;
	OP* op_other = op_first->op_sibling;
	if (o->op_private & OPpWHILE_AND_ONCE) {
	    /*
	          ...
	      label1:
	          <op_other>
	          <op_first>
	          or                   label1
	          ...
	    */
	    save_branch_point(bpp, &(cLOGOPo->op_other_instr));
	    add_op(bpp, op_other, &kid_may_constant_fold, 0);
	    add_op(bpp, op_first, &kid_may_constant_fold, 0);
	    append_instruction(bpp, o, OP_OR);
	}
	else {
	    /*
	          ...
	          instr_jump           label2
	      label1:
	          <op_other>
	      label2:
	          <op_first>
	          or                   label1
	          ...
	    */
	    int start_idx;
	    start_idx = bpp->idx;
	    append_instruction_x(bpp, NULL, OP_INSTR_JUMP, NULL, NULL);

	    save_branch_point(bpp, &(cLOGOPo->op_other_instr));
	    add_op(bpp, op_other, &kid_may_constant_fold, 0);

	    save_instr_from_to_pparg(bpp, start_idx, bpp->idx);
	    add_op(bpp, op_first, &kid_may_constant_fold, 0);

	    append_instruction(bpp, o, OP_OR);
	}

	break;
    }
    case OP_AND:
    case OP_OR:
    case OP_DOR: {
	/*
	      ...
	      <op_first>
	      o->op_type            label1
	      <op_other>
	  label1:
	      ...
	*/
	OP* op_first = cLOGOPo->op_first;
	OP* op_other = op_first->op_sibling;
	bool cond_may_constant_fold = TRUE;
	int addop_cond_flags = 0;
	assert((PL_opargs[o->op_type] & OA_CLASS_MASK) == OA_LOGOP);

	if ((o->op_flags & OPf_WANT) == OPf_WANT_VOID)
	    addop_cond_flags |= ADDOPf_BOOLEANCONTEXT;
	add_op(bpp, op_first, &cond_may_constant_fold, addop_cond_flags);

	if (cond_may_constant_fold) {
	    SV* const constsv = *(svp_const_instruction(bpp, bpp->idx-1));
	    bool const cond_true = ((o->op_type == OP_AND &&  SvTRUE(constsv)) ||
		(o->op_type == OP_OR  && !SvTRUE(constsv)) ||
		(o->op_type == OP_DOR && !SvOK(constsv)));
	    if (cond_true) {
		bpp->idx--;
		add_op(bpp, op_other, &kid_may_constant_fold, 0);
	    }
	    break;
	}
		
	append_instruction(bpp, o, o->op_type);
	add_op(bpp, op_other, &kid_may_constant_fold, 0);
	save_branch_point(bpp, &(cLOGOPo->op_other_instr));
	break;
    }
    case OP_ANDASSIGN:
    case OP_ORASSIGN:
    case OP_DORASSIGN: {
	/*
	      ...
	      <op_first>
	      o->op_type            label1
	      <op_other>
	  label1:
	      ...
	*/
	OP* op_first = cLOGOPo->op_first;
	OP* op_other = op_first->op_sibling;
	assert((PL_opargs[o->op_type] & OA_CLASS_MASK) == OA_LOGOP);

	add_op(bpp, op_first, &kid_may_constant_fold, 0);
	append_instruction(bpp, o, o->op_type);
	add_op(bpp, op_other, &kid_may_constant_fold, 0);
	save_branch_point(bpp, &(cLOGOPo->op_other_instr));
	break;
    }
    case OP_ONCE:
    {
	/*
	      ...
	      o->op_type            label1
	      <op_first>
	      instr_jump            label2
	  label1:
	      <op_other>
	  label2:
	      ...
	*/
	int start_idx;
	OP* op_first = cLOGOPo->op_first;
	OP* op_other = op_first->op_sibling;
	assert((PL_opargs[o->op_type] & OA_CLASS_MASK) == OA_LOGOP);

	append_instruction(bpp, o, o->op_type);

	add_op(bpp, op_first, &kid_may_constant_fold, 0);

	start_idx = bpp->idx;
	append_instruction_x(bpp, NULL, OP_INSTR_JUMP, NULL, NULL);
		    
	save_branch_point(bpp, &(cLOGOPo->op_other_instr));
	add_op(bpp, op_other, &kid_may_constant_fold, 0);
	save_instr_from_to_pparg(bpp, start_idx, bpp->idx);

	break;
    }
    case OP_ENTERTRY: {
	/*
	      ...
	      pp_entertry     label1
	      <o->op_first>
	      pp_leavetry
	  label1:
	      ...
	*/
	append_instruction(bpp, o, OP_ENTERTRY);
	add_op(bpp, cLOGOPo->op_first, &kid_may_constant_fold, 0);
	append_instruction(bpp, o, OP_LEAVETRY);
	save_branch_point(bpp, &(cLOGOPo->op_other_instr));
	break;
    }
    case OP_RANGE: {
	UNOP* flip = cUNOPx(cLOGOPo->op_first);

	if ((o->op_flags & OPf_WANT) == OPf_WANT_LIST) {
	    /*
	          ...
	          <o->op_first->op_first>
	          <o->op_first->op_first->op_sibling>
	          flop
	          ...
	    */
		  
	    int start_idx = bpp->idx;

	    add_op(bpp, flip->op_first, &kid_may_constant_fold, 0);
	    add_op(bpp, flip->op_first->op_sibling, &kid_may_constant_fold, 0);
	    append_instruction(bpp, o, OP_FLOP);
		
	    if (kid_may_constant_fold) {
		SV* constsv;
		append_instruction_x(bpp, NULL, OP_INSTR_END, NULL, NULL);
		constsv = instr_fold_constants(&(bpp->codeseq.xcodeseq_instructions[start_idx]), o, TRUE);
		if (constsv) {
		    bpp->idx = start_idx; /* FIXME remove pointer sets from bpp */
		    append_instruction_x(bpp, NULL, OP_INSTR_CONST_LIST, (void*)constsv, NULL);
		    Perl_av_create_and_push(aTHX_ &bpp->codeseq.xcodeseq_svs, constsv);
		}
		else {
		    bpp->idx--; /* remove OP_INSTR_END */
		}
	    }

	    break;
	}

	/*
	      ...
	      pp_range       label2
	  label1:
	      <o->op_first->op_first>
	      flip           label3
	  label2:
	      <o->op_first->op_first->op_sibling>
	      flop           label1
	  label3:
	      ...
	*/
		  
	{
	    int flip_instr_idx;
	    append_instruction(bpp, o, o->op_type);
	    add_op(bpp, flip->op_first, &kid_may_constant_fold, 0);
	    flip_instr_idx = bpp->idx;
	    append_instruction(bpp, o, OP_FLIP);
	    save_branch_point(bpp, &(cLOGOPo->op_other_instr));
	    add_op(bpp, flip->op_first->op_sibling, &kid_may_constant_fold, 0);
	    append_instruction(bpp, o, OP_FLOP);
	    save_instr_from_to_pparg(bpp, flip_instr_idx, bpp->idx);
	}
		
	break;
    }
    case OP_REGCOMP:
    {
	OP* op_first = cLOGOPo->op_first;
	if (op_first->op_type == OP_REGCRESET) {
	    append_instruction(bpp, op_first, op_first->op_type);
	    if (o->op_flags & OPf_STACKED)
		append_instruction(bpp, NULL, OP_PUSHMARK);
	    add_op(bpp, cUNOPx(op_first)->op_first, &kid_may_constant_fold, 0);
	}
	else {
	    if (o->op_flags & OPf_STACKED)
		append_instruction(bpp, NULL, OP_PUSHMARK);
	    add_op(bpp, op_first, &kid_may_constant_fold, 0);
	}
	append_instruction(bpp, o, o->op_type);
	break;
    }
    case OP_ENTERGIVEN:
    {
	/*
	      ...
	      <op_cond>
	      entergiven          label1
	      <op_block>
	  label1:
	      leavegiven
	      ...
	*/
	OP* op_cond = cLOGOPo->op_first;
	OP* op_block = op_cond->op_sibling;
	add_op(bpp, op_cond, &kid_may_constant_fold, 0);
	append_instruction(bpp, o, o->op_type);
	add_op(bpp, op_block, &kid_may_constant_fold, 0);
	save_branch_point(bpp, &(cLOGOPo->op_other_instr));
	append_instruction(bpp, o, OP_LEAVEGIVEN);

	break;
    }
    case OP_ENTERWHEN:
    {
	if (o->op_flags & OPf_SPECIAL) {
	    /*
	          ...
	          enterwhen          label1
	          <op_block>
	      label1:
	          leavewhen
	          ...
	    */
	    OP* op_block = cLOGOPo->op_first;
	    append_instruction(bpp, o, o->op_type);
	    add_op(bpp, op_block, &kid_may_constant_fold, 0);
	    save_branch_point(bpp, &(cLOGOPo->op_other_instr));
	    append_instruction(bpp, o, OP_LEAVEWHEN);
	}
	else {
	    /*
	          ...
	          <op_cond>
	      enterwhen          label1
	          <op_block>
	      label1:
	          leavewhen
	          ...
	    */
	    OP* op_cond = cLOGOPo->op_first;
	    OP* op_block = op_cond->op_sibling;
	    add_op(bpp, op_cond, &kid_may_constant_fold, 0);
	    append_instruction(bpp, o, o->op_type);
	    add_op(bpp, op_block, &kid_may_constant_fold, 0);
	    save_branch_point(bpp, &(cLOGOPo->op_other_instr));
	    append_instruction(bpp, o, OP_LEAVEWHEN);
	}

	break;
    }
    case OP_SUBST:
    {
	/*
	      ...
	      <kids>
	      pp_subst       label1 label2
	      instr_jump     label3
	  label1:
	      substcont
	  label2:
	      <o->op_pmreplroot>
	  label3:
	      ...
	*/
		  
	int start_idx;
	OP* kid;

	for (kid=cUNOPo->op_first; kid; kid=kid->op_sibling)
	    add_op(bpp, kid, &kid_may_constant_fold, 0);

	append_instruction(bpp, o, o->op_type);

	start_idx = bpp->idx;
	append_instruction_x(bpp, NULL, OP_INSTR_JUMP, NULL, NULL);
		    
	save_branch_point(bpp, &(cPMOPo->op_pmreplroot_instr));
	append_instruction(bpp, cPMOPo->op_pmreplrootu.op_pmreplroot, OP_SUBSTCONT);

	save_branch_point(bpp, &(cPMOPo->op_pmreplstart_instr));
	if (cPMOPo->op_pmreplrootu.op_pmreplroot)
	    add_op(bpp, cPMOPo->op_pmreplrootu.op_pmreplroot, &kid_may_constant_fold, 0);

	save_branch_point(bpp, &(cPMOPo->op_subst_next_instr));

	save_instr_from_to_pparg(bpp, start_idx, bpp->idx);

	break;
    }
    case OP_SORT:
    {
	/*
	      ...
	      pp_pushmark
	      [kids]
	      pp_sort               label2
	      instr_jump            label1
          label2:
	      [op_block]
	      (finished)
	  label1:
              ...        
	*/
	OP* kid;
	append_instruction(bpp, NULL, OP_PUSHMARK);

	kid = (o->op_flags & OPf_STACKED && o->op_flags & OPf_SPECIAL) ? cUNOPo->op_first->op_sibling
	    : cUNOPo->op_first;
	for (; kid; kid=kid->op_sibling)
	    add_op(bpp, kid, &kid_may_constant_fold, 0);

      compile_sort_without_kids:
	{
	    int start_idx, sort_instr_idx;
	    bool has_block = (o->op_flags & OPf_STACKED && o->op_flags & OPf_SPECIAL);

	    sort_instr_idx = bpp->idx;
	    append_instruction(bpp, o, OP_SORT);
	    start_idx = bpp->idx;
	    append_instruction_x(bpp, NULL, OP_INSTR_JUMP, NULL, NULL);
	    if (has_block) {
		save_instr_from_to_pparg(bpp, sort_instr_idx, bpp->idx);
		add_op(bpp, cUNOPo->op_first, &kid_may_constant_fold, 0);
		append_instruction_x(bpp, NULL, OP_INSTR_END, NULL, NULL);
	    }
	    save_instr_from_to_pparg(bpp, start_idx, bpp->idx);
	}
	break;
    }
    case OP_FORMLINE:
    {
	/*
	      ...
	  label1:
	      pp_pushmark
	      <o->children>
	      o->op_type          label1
	      ...
	*/
	save_branch_point(bpp, &(o->op_unstack_instr));
	append_instruction(bpp, NULL, OP_PUSHMARK);
	add_kids(bpp, o, &kid_may_constant_fold);
	append_instruction(bpp, o, o->op_type);
	break;
    }
    case OP_RV2SV:
    {
	if (cUNOPo->op_first->op_type == OP_GV &&
	    !(cUNOPo->op_private & OPpDEREF)) {
	    GV* gv = cGVOPx_gv(cUNOPo->op_first);
	    append_instruction_x(bpp, o, OP_GVSV, (void*)gv, NULL);
	    break;
	}
	add_kids(bpp, o, &kid_may_constant_fold);
	append_instruction(bpp, o, o->op_type);
	break;
    }
    case OP_AELEM:
    {
	/*
	  [op_av]
	  [op_index]
	  o->op_type
	*/
	OP* op_av = cUNOPo->op_first;
	OP* op_index = op_av->op_sibling;
	bool index_is_constant = TRUE;
	int start_idx;
	start_idx = bpp->idx;
	add_op(bpp, op_av, &kid_may_constant_fold, 0);
	add_op(bpp, op_index, &index_is_constant, 0);
	kid_may_constant_fold = kid_may_constant_fold && index_is_constant;
	if (index_is_constant) {
	    if ((op_av->op_type == OP_PADAV || 
		    (op_av->op_type == OP_RV2AV && cUNOPx(op_av)->op_first->op_type == OP_GV)) &&
		!(o->op_private & (OPpLVAL_INTRO|OPpLVAL_DEFER|OPpDEREF|OPpMAYBE_LVSUB))
		) {
		/* Convert to AELEMFAST */
		SV* const constsv = *(svp_const_instruction(bpp, bpp->idx-1));
		SvIV_please(constsv);
		if (SvIOKp(constsv)) {
		    IV i = SvIV(constsv) - CopARYBASE_get(PL_curcop);
		    OP* op_arg = op_av->op_type == OP_PADAV ? op_av : cUNOPx(op_av)->op_first;
		    op_arg->op_flags |= o->op_flags & OPf_MOD;
		    op_arg->op_private |= o->op_private & OPpLVAL_DEFER;
		    bpp->idx = start_idx;
		    append_instruction_x(bpp, op_arg,
			OP_AELEMFAST, INT2PTR(void*, i), NULL);
		    break;
		}
	    }
	}
	append_instruction(bpp, o, o->op_type);
	break;
    }
    case OP_HELEM:
    {
	/*
	  [op_hv]
	  [op_index]
	  o->op_type
	*/
	OP* op_hv = cUNOPo->op_first;
	OP* op_key = op_hv->op_sibling;
	bool key_is_constant = TRUE;
	int start_idx;
	int flags = 0;
	if (o->op_flags & OPf_MOD)
	    flags |= INSTRf_MOD;
	if (o->op_private & OPpMAYBE_LVSUB)
	    flags |= INSTRf_HELEM_MAYBE_LVSUB;
	if (o->op_private & OPpLVAL_DEFER)
	    flags |= INSTRf_HELEM_LVAL_DEFER;
	if (o->op_private & OPpLVAL_INTRO)
	    flags |= INSTRf_LVAL_INTRO;
	if (o->op_flags & OPf_SPECIAL)
	    flags |= INSTRf_HELEM_SPECIAL;
	flags |= (o->op_private & OPpDEREF);

	start_idx = bpp->idx;
	add_op(bpp, op_hv, &kid_may_constant_fold, 0);
	add_op(bpp, op_key, &key_is_constant, 0);
	kid_may_constant_fold = kid_may_constant_fold && key_is_constant;
	if (key_is_constant) {
	    SV ** const keysvp = svp_const_instruction(bpp, bpp->idx-1);
	    STRLEN keylen;
	    const char* key = SvPV_const(*keysvp, keylen);
	    SV* shared_keysv = newSVpvn_share(key,
		                              SvUTF8(*keysvp) ? -(I32)keylen : (I32)keylen,
		                              0);
	    SvREFCNT_dec(*keysvp);
	    *keysvp = shared_keysv;
	}
	append_instruction_x(bpp, o, o->op_type, (void*)flags, NULL);
	break;
    }
    case OP_DELETE:
    {
	if (o->op_private & OPpSLICE)
	    append_instruction(bpp, NULL, OP_PUSHMARK);
	add_kids(bpp, o, &kid_may_constant_fold);
	append_instruction(bpp, o, OP_DELETE);
	break;
    }
    case OP_LSLICE:
    {
	/*
	      pp_pushmark
	      [op_subscript]
	      pp_pushmark
	      [op_listval]
	      pp_lslice
	*/
	OP* op_subscript = cBINOPo->op_first;
	OP* op_listval = op_subscript->op_sibling;
	append_instruction(bpp, NULL, OP_PUSHMARK);
	add_op(bpp, op_subscript, &kid_may_constant_fold, 0);
	append_instruction(bpp, NULL, OP_PUSHMARK);
	add_op(bpp, op_listval, &kid_may_constant_fold, 0);
	append_instruction(bpp, o, OP_LSLICE);
	break;
    }
    case OP_RV2HV: {
	if (boolean_context) {
	    o->op_flags |= ( OPf_REF | OPf_MOD );
	    add_kids(bpp, o, &kid_may_constant_fold);
	    append_instruction(bpp, o, OP_RV2HV);
	    append_instruction(bpp, NULL, OP_BOOLKEYS);
	    break;
	}
	goto compile_default;
    }
    case OP_REPEAT:
    {
	if (o->op_private & OPpREPEAT_DOLIST)
	    append_instruction(bpp, NULL, OP_PUSHMARK);
	add_kids(bpp, o, &kid_may_constant_fold);
	append_instruction(bpp, o, OP_REPEAT);
	break;
    }
    case OP_NULL:
    case OP_SCALAR:
    case OP_LINESEQ:
    case OP_SCOPE:
    {
	add_kids(bpp, o, &kid_may_constant_fold);
	break;
    }
    case OP_NEXTSTATE:
    {
	/* Two NEXTSTATEs in a row serve no purpose. Except if they happen
	   to carry two labels. For now, take the easier option, and skip
	   this optimisation if the first NEXTSTATE has a label.  */
	if (o->op_next && o->op_next->op_type == OP_NEXTSTATE
	     && !CopLABEL((COP*)o)
	    )
	    break;
	S_append_instruction(codeseq, bpp, o, o->op_type);
	PL_curcop = ((COP*)o);
	break;
    }
    case OP_DBSTATE:
    {
	append_instruction(bpp, o, o->op_type);
	PL_curcop = ((COP*)o);
	break;
    }
    case OP_SASSIGN: {
	OP* op_right = cBINOPo->op_first;
	OP* op_left = cBINOPo->op_last;
    	if (op_left && op_left->op_type == OP_PADSV
    	    && !(op_left->op_private & OPpLVAL_INTRO)
	    && (PL_opargs[op_right->op_type] & OA_TARGLEX)
	    && (!(op_right->op_flags & OPf_STACKED))
	    ) {
	    assert(!(op_left->op_flags & OPf_STACKED));
	    if (PL_opargs[op_right->op_type] & OA_MARK)
		append_instruction(bpp, NULL, OP_PUSHMARK);
	    add_kids(bpp, op_right, &kid_may_constant_fold);
	    append_instruction_x(bpp, op_right, op_right->op_type, 
		(void*)INSTRf_TARG_IN_ARG2, (void*)op_left->op_targ);
	    break;
	}
	goto compile_default;
    }
    case OP_AASSIGN:
    {
	OP* op_right = cBINOPo->op_first;
	OP* op_left = op_right->op_sibling;

	OP* inplace_av_op = is_inplace_av(o);
	if (inplace_av_op) {
	    if (inplace_av_op->op_type == OP_SORT) {
		inplace_av_op->op_private |= OPpSORT_INPLACE;
	    
		append_instruction(bpp, NULL, OP_PUSHMARK);
		append_instruction(bpp, NULL, OP_PUSHMARK);
		if (inplace_av_op->op_flags & OPf_STACKED && !(inplace_av_op->op_flags & OPf_SPECIAL))
		    add_op(bpp, cLISTOPx(inplace_av_op)->op_first, &kid_may_constant_fold, 0);
		add_op(bpp, op_left, &kid_may_constant_fold, 0);

		o = inplace_av_op;
		goto compile_sort_without_kids;
	    }
	    assert(inplace_av_op->op_type == OP_REVERSE);
	    inplace_av_op->op_private |= OPpREVERSE_INPLACE;
	    append_instruction(bpp, NULL, OP_PUSHMARK);
	    append_instruction(bpp, NULL, OP_PUSHMARK);
	    add_op(bpp, op_left, &kid_may_constant_fold, 0);
	    append_instruction(bpp, inplace_av_op, OP_REVERSE);
	    break;
	}
	append_instruction(bpp, NULL, OP_PUSHMARK);
	add_op(bpp, op_right, &kid_may_constant_fold, 0);
	append_instruction(bpp, NULL, OP_PUSHMARK);
	add_op(bpp, op_left, &kid_may_constant_fold, 0);
	append_instruction(bpp, o, OP_AASSIGN);
	break;
    }
    case OP_STRINGIFY:
    {
	if (cUNOPo->op_first->op_type == OP_CONCAT) {
	    add_op(bpp, cUNOPo->op_first, &kid_may_constant_fold, 0);
	    break;
	}
	goto compile_default;
    }
    case OP_CONCAT:
    {
	if ((o->op_flags & OPf_STACKED) && cBINOPo->op_last->op_type == OP_READLINE) {
	    /* 	/\* Turn "$a .= <FH>" into an OP_RCATLINE. AMS 20010917 *\/ */
	    add_op(bpp, cBINOPo->op_first, &kid_may_constant_fold, 0);
	    add_kids(bpp, cBINOPo->op_last, &kid_may_constant_fold);
	    cBINOPo->op_last->op_type = OP_RCATLINE;
	    cBINOPo->op_last->op_flags |= OPf_STACKED;
	    append_instruction(bpp, cBINOPo->op_last, OP_RCATLINE);
	    kid_may_constant_fold = FALSE;
	    break;
	}
	goto compile_default;
    }
    case OP_LIST: {
	if ((o->op_flags & OPf_WANT) == OPf_WANT_LIST) {
	    /* don't bother with the pushmark and the pp_list instruction in list context */
	    add_kids(bpp, o, &kid_may_constant_fold);
	    break;
	}
	goto compile_default;
    }

    case OP_PADSV: {
	int flags = 0;
	if (o->op_flags & OPf_MOD)
	    flags |= INSTRf_MOD;
	if (o->op_private & OPpLVAL_INTRO)
	    flags |= INSTRf_LVAL_INTRO;
	if (o->op_private & OPpPAD_STATE)
	    flags |= INSTRf_PAD_STATE;
	flags |= o->op_private & OPpDEREF;
	    
	append_instruction_x(bpp, o, o->op_type, (void*)flags, (void*)o->op_targ);
	break;
    }

    case OP_STUB:
	if ((o->op_flags & OPf_WANT) == OPf_WANT_LIST) {
	    break; /* Scalar stub must produce undef.  List stub is noop */
	}
	goto compile_default;

    default: {
      compile_default:
	if (PL_opargs[o->op_type] & OA_MARK)
	    append_instruction(bpp, NULL, OP_PUSHMARK);
	add_kids(bpp, o, &kid_may_constant_fold);
	append_instruction(bpp, o, o->op_type);
	break;
    }
    }

    switch (o->op_type) {
    case OP_CONST:
    case OP_SCALAR:
    case OP_NULL:
	break;
    case OP_UCFIRST:
    case OP_LCFIRST:
    case OP_UC:
    case OP_LC:
    case OP_SLT:
    case OP_SGT:
    case OP_SLE:
    case OP_SGE:
    case OP_SCMP:
	/* XXX what about the numeric ops? */
	if (PL_hints & HINT_LOCALE)
	    kid_may_constant_fold = FALSE;
	break;
    default:
	kid_may_constant_fold = kid_may_constant_fold && (PL_opargs[o->op_type] & OA_FOLDCONST) != 0;
	break;
    }

    if (kid_may_constant_fold && bpp->idx > start_idx + 1) {
    	SV* constsv;
	append_instruction_x(bpp, NULL, OP_INSTR_END, NULL, NULL);
    	constsv = instr_fold_constants(&(bpp->codeseq.xcodeseq_instructions[start_idx]), o, FALSE);
    	if (constsv) {
    	    bpp->idx = start_idx; /* FIXME remove pointer sets from bpp */
    	    SvREADONLY_on(constsv);
    	    append_instruction_x(bpp, NULL, OP_INSTR_CONST, (void*)constsv, NULL);
    	    Perl_av_create_and_push(aTHX_ &bpp->codeseq.xcodeseq_svs, constsv);
    	}
	else {
	    /* constant folding failed */
	    kid_may_constant_fold = FALSE;
	    bpp->idx--; /* remove OP_INSTR_END */
	}
    }

    *may_constant_fold = *may_constant_fold && kid_may_constant_fold;
    bpp->recursion--;
}

/*
=item compile_op
Compiles to op into the codeseq, assumes the pad is setup correctly
*/
void
Perl_compile_op(pTHX_ OP* startop, CODESEQ* codeseq)
{
    dSP;

    CODEGEN_PAD bpp;

    /* preserve current state */
    PUSHSTACKi(PERLSI_COMPILE);
    ENTER;
    SAVETMPS;

    save_scalar(PL_errgv);
    SAVEVPTR(PL_curcop);

    /* create scratch pad */
    Newx(bpp.op_instrpp_list, 128, OP_INSTRPP);
    bpp.codeseq.xcodeseq_size = 12;
    Newx(bpp.codeseq.xcodeseq_instructions, bpp.codeseq.xcodeseq_size, INSTRUCTION);
    bpp.codeseq.xcodeseq_svs = NULL;
    bpp.idx = 0;
    bpp.op_instrpp_append = bpp.op_instrpp_list;
    bpp.op_instrpp_end = bpp.op_instrpp_list + 128;
    bpp.branch_point_to_pparg_list = NULL;
    bpp.branch_point_to_pparg_end = NULL;
    bpp.branch_point_to_pparg_append = NULL;

    PERL_ARGS_ASSERT_COMPILE_OP;

    {
	/* actually compile */
	bool may_constant_fold = TRUE;
	add_op(&bpp, startop, &may_constant_fold, 0);

	append_instruction_x(&bpp, NULL, OP_INSTR_END, NULL, NULL);
    }

    /* copy codeseq from the pad to the actual object */
    codeseq->xcodeseq_size = bpp.idx + 1;
    Renew(codeseq->xcodeseq_instructions, codeseq->xcodeseq_size, INSTRUCTION);
    Copy(bpp.codeseq.xcodeseq_instructions, codeseq->xcodeseq_instructions, codeseq->xcodeseq_size, INSTRUCTION);
    codeseq->xcodeseq_svs = bpp.codeseq.xcodeseq_svs;
    
    /* Final NULL instruction */
    codeseq->xcodeseq_instructions[bpp.idx].instr_ppaddr = NULL;

    {
	/* resolve instruction pointers */
	OP_INSTRPP* i;
	for (i=bpp.op_instrpp_list; i<bpp.op_instrpp_append; i++) {
	    assert(i->instr_idx != -1);
	    if (i->instrpp)
		*(i->instrpp) = &(codeseq->xcodeseq_instructions[i->instr_idx]);
	}
    }
    
    {
	BRANCH_POINT_TO_PPARG* i;
	for (i=bpp.branch_point_to_pparg_list; i<bpp.branch_point_to_pparg_append; i++) {
	    codeseq->xcodeseq_instructions[i->instr_from_index].instr_arg1 = &codeseq->xcodeseq_instructions[i->instr_to_index];
	}
    }

    DEBUG_G(codeseq_dump(codeseq));

    Safefree(bpp.op_instrpp_list);
    Safefree(bpp.branch_point_to_pparg_list);
    Safefree(bpp.codeseq.xcodeseq_instructions);

    /* restore original state */
    FREETMPS ;
    LEAVE ;
    POPSTACK;
}

/* Checks if o acts as an in-place operator on an array. o points to the
 * assign op. Returns the the in-place operator if available or NULL otherwise */

OP *
S_is_inplace_av(pTHX_ OP *o) {
    OP *oright = cBINOPo->op_first;
    OP *oleft = cBINOPo->op_first->op_sibling;
    OP *sortop;

    PERL_ARGS_ASSERT_IS_INPLACE_AV;

    /* Only do inplace sort in void context */
    assert(o->op_type == OP_AASSIGN);

    if ((o->op_flags & OPf_WANT) != OPf_WANT_VOID)
	return NULL;

    /* check that the sort is the first arg on RHS of assign */

    assert(oright->op_type == OP_LIST);
    oright = cLISTOPx(oright)->op_first;
    if (!oright || oright->op_sibling)
	return NULL;
    if (oright->op_type != OP_SORT && oright->op_type != OP_REVERSE)
	return NULL;
    sortop = oright;
    oright = cLISTOPx(oright)->op_first;
    if (sortop->op_flags & OPf_STACKED)
	oright = oright->op_sibling; /* skip block */

    if (!oright || oright->op_sibling)
	return NULL;

    /* Check that the LHS and RHS are both assignments to a variable */
    if (!oright ||
    	(oright->op_type != OP_RV2AV && oright->op_type != OP_PADAV)
    	|| (oright->op_private & OPpLVAL_INTRO)
    )
    	return NULL;

    assert(oleft->op_type == OP_LIST);
    oleft = cLISTOPx(oleft)->op_first;
    if (!oleft || oleft->op_sibling)
	return NULL;

    if ((oleft->op_type != OP_PADAV && oleft->op_type != OP_RV2AV)
	|| (oleft->op_private & OPpLVAL_INTRO)
	)
	return NULL;

    /* check the array is the same on both sides */
    if (oleft->op_type == OP_RV2AV) {
    	if (oright->op_type != OP_RV2AV
    	    || !cUNOPx(oright)->op_first
    	    || cUNOPx(oright)->op_first->op_type != OP_GV
    	    || cGVOPx_gv(cUNOPx(oleft)->op_first) !=
    	       cGVOPx_gv(cUNOPx(oright)->op_first)
    	)
    	    return NULL;
    }
    else if (oright->op_type != OP_PADAV
    	|| oright->op_targ != oleft->op_targ
    )
    	return NULL;

    return sortop;
}

SV**
S_svp_const_instruction(pTHX_ CODEGEN_PAD *bpp, int instr_index)
{
    INSTRUCTION* instr = &bpp->codeseq.xcodeseq_instructions[instr_index];
    PERL_ARGS_ASSERT_SVP_CONST_INSTRUCTION;
    PERL_UNUSED_VAR(bpp);
    if (instr->instr_op) {
	assert(instr->instr_op->op_type == OP_CONST);
	return cSVOPx_svp(instr->instr_op);
    }
    else {
	return (SV**)& instr->instr_arg1;
    }
}

void
Perl_compile_cv(pTHX_ CV* cv)
{
    PAD* oldpad;
    AV * const cvpad = (AV *)*av_fetch(CvPADLIST(cv), 1, FALSE);

    PERL_ARGS_ASSERT_COMPILE_CV;

    if (CvCODESEQ(cv))
	return;

    CvCODESEQ(cv) = new_codeseq();

    PAD_SAVE_LOCAL(oldpad, cvpad);

    compile_op(CvROOT(cv), CvCODESEQ(cv));

    PAD_RESTORE_LOCAL(oldpad);
}

/*
 * Local variables:
 * c-indentation-style: bsd
 * c-basic-offset: 4
 * indent-tabs-mode: t
 * End:
 *
 * ex: set ts=8 sts=4 sw=4 noet:
 */
