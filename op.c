#line 2 "op.c"
/*    op.c
 *
 *    Copyright (C) 1991, 1992, 1993, 1994, 1995, 1996, 1997, 1998, 1999, 2000,
 *    2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008 by Larry Wall and others
 *
 *    You may distribute under the terms of either the GNU General Public
 *    License or the Artistic License, as specified in the README file.
 *
 */

/*
 * 'You see: Mr. Drogo, he married poor Miss Primula Brandybuck.  She was
 *  our Mr. Bilbo's first cousin on the mother's side (her mother being the
 *  youngest of the Old Took's daughters); and Mr. Drogo was his second
 *  cousin.  So Mr. Frodo is his first *and* second cousin, once removed
 *  either way, as the saying is, if you follow me.'       --the Gaffer
 *
 *     [p.23 of _The Lord of the Rings_, I/i: "A Long-Expected Party"]
 */

/* This file contains the functions that create, manipulate and optimize
 * the OP structures that hold a compiled perl program.
 *
 * A Perl program is compiled into a tree of OPs. Each op contains
 * structural pointers (eg to its siblings and the next op in the
 * execution sequence), a pointer to the function that would execute the
 * op, plus any data specific to that op. For example, an OP_CONST op
 * points to the pp_const() function and to an SV containing the constant
 * value. When pp_const() is executed, its job is to push that SV onto the
 * stack.
 *
 * OPs are mainly created by the newFOO() functions, which are mainly
 * called from the parser (in perly.y) as the code is parsed. For example
 * the Perl code $a + $b * $c would cause the equivalent of the following
 * to be called (oversimplifying a bit):
 *
 *  newBINOP(OP_ADD, flags,
 *	newSVREF($a),
 *	newBINOP(OP_MULTIPLY, flags, newSVREF($b), newSVREF($c))
 *  )
 *
 * Note that during the build of miniperl, a temporary copy of this file
 * is made, called opmini.c.
 */

/*
Perl's compiler is essentially a 3-pass compiler with interleaved phases:

    A bottom-up pass
    A top-down pass
    An execution-order pass

The bottom-up pass is represented by all the "newOP" routines and
the ck_ routines.  The bottom-upness is actually driven by yacc.
So at the point that a ck_ routine fires, we have no idea what the
context is, either upward in the syntax tree, or either forward or
backward in the execution order.  (The bottom-up parser builds that
part of the execution order it knows about, but if you follow the "next"
links around, you'll find it's actually a closed loop through the
top level node.)

Whenever the bottom-up parser gets to a node that supplies context to
its components, it invokes that portion of the top-down pass that applies
to that part of the subtree (and marks the top node as processed, so
if a node further up supplies context, it doesn't have to take the
plunge again).  As a particular subcase of this, as the new node is
built, it takes all the closed execution loops of its subcomponents
and links them into a new closed loop for the higher level node.  But
it's still not the real execution order.

The actual execution order is not known till we get a grammar reduction
to a top-level unit like a subroutine or file that will be called by
"name" rather than via a "next" pointer.  At that point, we can call
into peep() to do that code's portion of the 3rd pass.  It has to be
recursive, but it's recursive on basic blocks, not on tree nodes.
*/

/* To implement user lexical pragmas, there needs to be a way at run time to
   get the compile time state of %^H for that block.  Storing %^H in every
   block (or even COP) would be very expensive, so a different approach is
   taken.  The (running) state of %^H is serialised into a tree of HE-like
   structs.  Stores into %^H are chained onto the current leaf as a struct
   refcounted_he * with the key and the value.  Deletes from %^H are saved
   with a value of PL_sv_placeholder.  The state of %^H at any point can be
   turned back into a regular HV by walking back up the tree from that point's
   leaf, ignoring any key you've already seen (placeholder or not), storing
   the rest into the HV structure, then removing the placeholders. Hence
   memory is only used to store the %^H deltas from the enclosing COP, rather
   than the entire %^H on each COP.

   To cause actions on %^H to write out the serialisation records, it has
   magic type 'H'. This magic (itself) does nothing, but its presence causes
   the values to gain magic type 'h', which has entries for set and clear.
   C<Perl_magic_sethint> updates C<PL_compiling.cop_hints_hash> with a store
   record, with deletes written by C<Perl_magic_clearhint>. C<SAVEHINTS>
   saves the current C<PL_compiling.cop_hints_hash> on the save stack, so that
   it will be correctly restored when any inner compiling scope is exited.
*/

#include "EXTERN.h"
#define PERL_IN_OP_C
#include "perl.h"
#include "keywords.h"

#define CALL_PEEP(o) PL_peepp(aTHX_ o)
#define CALL_RPEEP(o) PL_rpeepp(aTHX_ o)
#define CALL_OPFREEHOOK(o) if (PL_opfreehook) PL_opfreehook(aTHX_ o)

#if defined(PL_OP_SLAB_ALLOC)

#ifdef PERL_DEBUG_READONLY_OPS
#  define PERL_SLAB_SIZE 4096
#  include <sys/mman.h>
#endif

#ifndef PERL_SLAB_SIZE
#define PERL_SLAB_SIZE 2048
#endif

void *
Perl_Slab_Alloc(pTHX_ size_t sz)
{
    dVAR;
    /*
     * To make incrementing use count easy PL_OpSlab is an I32 *
     * To make inserting the link to slab PL_OpPtr is I32 **
     * So compute size in units of sizeof(I32 *) as that is how Pl_OpPtr increments
     * Add an overhead for pointer to slab and round up as a number of pointers
     */
    sz = (sz + 2*sizeof(I32 *) -1)/sizeof(I32 *);
    if ((PL_OpSpace -= sz) < 0) {
#ifdef PERL_DEBUG_READONLY_OPS
	/* We need to allocate chunk by chunk so that we can control the VM
	   mapping */
	PL_OpPtr = (I32**) mmap(0, PERL_SLAB_SIZE*sizeof(I32*), PROT_READ|PROT_WRITE,
			MAP_ANON|MAP_PRIVATE, -1, 0);

	DEBUG_m(PerlIO_printf(Perl_debug_log, "mapped %lu at %p\n",
			      (unsigned long) PERL_SLAB_SIZE*sizeof(I32*),
			      PL_OpPtr));
	if(PL_OpPtr == MAP_FAILED) {
	    perror("mmap failed");
	    abort();
	}
#else

        PL_OpPtr = (I32 **) PerlMemShared_calloc(PERL_SLAB_SIZE,sizeof(I32*)); 
#endif
    	if (!PL_OpPtr) {
	    return NULL;
	}
	/* We reserve the 0'th I32 sized chunk as a use count */
	PL_OpSlab = (I32 *) PL_OpPtr;
	/* Reduce size by the use count word, and by the size we need.
	 * Latter is to mimic the '-=' in the if() above
	 */
	PL_OpSpace = PERL_SLAB_SIZE - (sizeof(I32)+sizeof(I32 **)-1)/sizeof(I32 **) - sz;
	/* Allocation pointer starts at the top.
	   Theory: because we build leaves before trunk allocating at end
	   means that at run time access is cache friendly upward
	 */
	PL_OpPtr += PERL_SLAB_SIZE;

#ifdef PERL_DEBUG_READONLY_OPS
	/* We remember this slab.  */
	/* This implementation isn't efficient, but it is simple. */
	PL_slabs = (I32**) realloc(PL_slabs, sizeof(I32**) * (PL_slab_count + 1));
	PL_slabs[PL_slab_count++] = PL_OpSlab;
	DEBUG_m(PerlIO_printf(Perl_debug_log, "Allocate %p\n", PL_OpSlab));
#endif
    }
    assert( PL_OpSpace >= 0 );
    /* Move the allocation pointer down */
    PL_OpPtr   -= sz;
    assert( PL_OpPtr > (I32 **) PL_OpSlab );
    *PL_OpPtr   = PL_OpSlab;	/* Note which slab it belongs to */
    (*PL_OpSlab)++;		/* Increment use count of slab */
    assert( PL_OpPtr+sz <= ((I32 **) PL_OpSlab + PERL_SLAB_SIZE) );
    assert( *PL_OpSlab > 0 );
    return (void *)(PL_OpPtr + 1);
}

#ifdef PERL_DEBUG_READONLY_OPS
void
Perl_pending_Slabs_to_ro(pTHX) {
    /* Turn all the allocated op slabs read only.  */
    U32 count = PL_slab_count;
    I32 **const slabs = PL_slabs;

    /* Reset the array of pending OP slabs, as we're about to turn this lot
       read only. Also, do it ahead of the loop in case the warn triggers,
       and a warn handler has an eval */

    PL_slabs = NULL;
    PL_slab_count = 0;

    /* Force a new slab for any further allocation.  */
    PL_OpSpace = 0;

    while (count--) {
	void *const start = slabs[count];
	const size_t size = PERL_SLAB_SIZE* sizeof(I32*);
	if(mprotect(start, size, PROT_READ)) {
	    Perl_warn(aTHX_ "mprotect for %p %lu failed with %d",
		      start, (unsigned long) size, errno);
	}
    }

    free(slabs);
}

STATIC void
S_Slab_to_rw(pTHX_ void *op)
{
    I32 * const * const ptr = (I32 **) op;
    I32 * const slab = ptr[-1];

    PERL_ARGS_ASSERT_SLAB_TO_RW;

    assert( ptr-1 > (I32 **) slab );
    assert( ptr < ( (I32 **) slab + PERL_SLAB_SIZE) );
    assert( *slab > 0 );
    if(mprotect(slab, PERL_SLAB_SIZE*sizeof(I32*), PROT_READ|PROT_WRITE)) {
	Perl_warn(aTHX_ "mprotect RW for %p %lu failed with %d",
		  slab, (unsigned long) PERL_SLAB_SIZE*sizeof(I32*), errno);
    }
}

OP *
Perl_op_refcnt_inc(pTHX_ OP *o)
{
    if(o) {
	Slab_to_rw(o);
	++o->op_targ;
    }
    return o;

}

PADOFFSET
Perl_op_refcnt_dec(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_OP_REFCNT_DEC;
    Slab_to_rw(o);
    return --o->op_targ;
}
#else
#  define Slab_to_rw(op)
#endif

void
Perl_Slab_Free(pTHX_ void *op)
{
    I32 * const * const ptr = (I32 **) op;
    I32 * const slab = ptr[-1];
    PERL_ARGS_ASSERT_SLAB_FREE;
    assert( ptr-1 > (I32 **) slab );
    assert( ptr < ( (I32 **) slab + PERL_SLAB_SIZE) );
    assert( *slab > 0 );
    Slab_to_rw(op);
    if (--(*slab) == 0) {
#  ifdef NETWARE
#    define PerlMemShared PerlMem
#  endif
	
#ifdef PERL_DEBUG_READONLY_OPS
	U32 count = PL_slab_count;
	/* Need to remove this slab from our list of slabs */
	if (count) {
	    while (count--) {
		if (PL_slabs[count] == slab) {
		    dVAR;
		    /* Found it. Move the entry at the end to overwrite it.  */
		    DEBUG_m(PerlIO_printf(Perl_debug_log,
					  "Deallocate %p by moving %p from %lu to %lu\n",
					  PL_OpSlab,
					  PL_slabs[PL_slab_count - 1],
					  PL_slab_count, count));
		    PL_slabs[count] = PL_slabs[--PL_slab_count];
		    /* Could realloc smaller at this point, but probably not
		       worth it.  */
		    if(munmap(slab, PERL_SLAB_SIZE*sizeof(I32*))) {
			perror("munmap failed");
			abort();
		    }
		    break;
		}
	    }
	}
#else
    PerlMemShared_free(slab);
#endif
	if (slab == PL_OpSlab) {
	    PL_OpSpace = 0;
	}
    }
}
#endif
/*
 * In the following definition, the ", (OP*)0" is just to make the compiler
 * think the expression is of the right type: croak actually does a Siglongjmp.
 */
#define CHECKOP(type,o) \
    ((PL_op_mask && PL_op_mask[type])				\
     ? ( op_free((OP*)o),					\
	 Perl_croak(aTHX_ "'%s' trapped by operation mask", PL_op_desc[type]),	\
	 (OP*)0 )						\
     : PL_check[type](aTHX_ (OP*)o))

#define RETURN_UNLIMITED_NUMBER (PERL_INT_MAX / 2)

STATIC const char*
S_gv_ename(pTHX_ GV *gv)
{
    SV* const tmpsv = sv_newmortal();

    PERL_ARGS_ASSERT_GV_ENAME;

    gv_efullname3(tmpsv, gv, NULL);
    return SvPV_nolen_const(tmpsv);
}

STATIC OP *
S_no_fh_allowed(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_NO_FH_ALLOWED;

    yyerror(Perl_form(aTHX_ "Missing comma after first argument to %s function",
		 OP_DESC(o)));
    return o;
}

STATIC OP *
S_too_few_arguments(pTHX_ OP *o, const char *name)
{
    PERL_ARGS_ASSERT_TOO_FEW_ARGUMENTS;

    yyerror(Perl_form(aTHX_ "Not enough arguments for %s", name));
    return o;
}

STATIC OP *
S_too_many_arguments(pTHX_ OP *o, const char *name)
{
    PERL_ARGS_ASSERT_TOO_MANY_ARGUMENTS;

    yyerror(Perl_form(aTHX_ "Too many arguments for %s", name));
    return o;
}

STATIC void
S_bad_type(pTHX_ I32 n, const char *t, const char *name, const OP *kid)
{
    PERL_ARGS_ASSERT_BAD_TYPE;

    yyerror(Perl_form(aTHX_ "Type of arg %d to %s must be %s (not %s)",
		 (int)n, name, t, OP_DESC(kid)));
}

STATIC void
S_no_bareword_allowed(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_NO_BAREWORD_ALLOWED;

    if (PL_madskills)
	return;		/* various ok barewords are hidden in extra OP_NULL */
    o->op_private &= ~OPpCONST_STRICT;
    qerror(Perl_mess(aTHX_
		     "Bareword \"%"SVf"\" not allowed while \"strict subs\" in use",
		     SVfARG(cSVOPo_sv)));
}

/* "register" allocation */

PADOFFSET
Perl_allocmy(pTHX_ const char *const name, const STRLEN len, const U32 flags)
{
    dVAR;
    PADOFFSET off;
    const bool is_our = (PL_parser->in_my == KEY_our);

    PERL_ARGS_ASSERT_ALLOCMY;

    if (flags)
	Perl_croak(aTHX_ "panic: allocmy illegal flag bits 0x%" UVxf,
		   (UV)flags);

    /* Until we're using the length for real, cross check that we're being
       told the truth.  */
    assert(strlen(name) == len);

    /* complain about "my $<special_var>" etc etc */
    if (len &&
	!(is_our ||
	  isALPHA(name[1]) ||
	  (USE_UTF8_IN_NAMES && UTF8_IS_START(name[1])) ||
	  (name[1] == '_' && (*name == '$' || len > 2))))
    {
	/* name[2] is true if strlen(name) > 2  */
	if (!isPRINT(name[1]) || strchr("\t\n\r\f", name[1])) {
	    yyerror(Perl_form(aTHX_ "Can't use global %c^%c%.*s in \"%s\"",
			      name[0], toCTRL(name[1]), (int)(len - 2), name + 2,
			      PL_parser->in_my == KEY_state ? "state" : "my"));
	} else {
	    yyerror(Perl_form(aTHX_ "Can't use global %.*s in \"%s\"", (int) len, name,
			      PL_parser->in_my == KEY_state ? "state" : "my"));
	}
    }

    /* allocate a spare slot and store the name in that slot */

    off = pad_add_name(name, len,
		       is_our ? padadd_OUR :
		       PL_parser->in_my == KEY_state ? padadd_STATE : 0,
		    PL_parser->in_my_stash,
		    (is_our
		        /* $_ is always in main::, even with our */
			? (PL_curstash && !strEQ(name,"$_") ? PL_curstash : PL_defstash)
			: NULL
		    )
    );
    /* anon sub prototypes contains state vars should always be cloned,
     * otherwise the state var would be shared between anon subs */

    if (PL_parser->in_my == KEY_state && CvANON(PL_compcv))
	CvCLONE_on(PL_compcv);

    return off;
}

/* free the body of an op without examining its contents.
 * Always use this rather than FreeOp directly */

static void
S_op_destroy(pTHX_ OP *o)
{
    if (o->op_latefree) {
	o->op_latefreed = 1;
	return;
    }
    FreeOp(o);
}

#ifdef USE_ITHREADS
#  define forget_pmop(a,b)	S_forget_pmop(aTHX_ a,b)
#else
#  define forget_pmop(a,b)	S_forget_pmop(aTHX_ a)
#endif

/* Destructor */

void
Perl_op_free(pTHX_ OP *o)
{
    dVAR;
    OPCODE type;

    if (!o)
	return;
    if (o->op_latefreed) {
	if (o->op_latefree)
	    return;
	goto do_free;
    }

    type = o->op_type;
    if (o->op_private & OPpREFCOUNTED) {
	switch (type) {
	case OP_LEAVESUB:
	case OP_LEAVESUBLV:
	case OP_LEAVEEVAL:
	case OP_LEAVE:
	case OP_SCOPE:
	case OP_LEAVEWRITE:
	    {
	    PADOFFSET refcnt;
	    OP_REFCNT_LOCK;
	    refcnt = OpREFCNT_dec(o);
	    OP_REFCNT_UNLOCK;
	    if (refcnt) {
		/* Need to find and remove any pattern match ops from the list
		   we maintain for reset().  */
		find_and_forget_pmops(o);
		return;
	    }
	    }
	    break;
	default:
	    break;
	}
    }

    /* Call the op_free hook if it has been set. Do it now so that it's called
     * at the right time for refcounted ops, but still before all of the kids
     * are freed. */
    CALL_OPFREEHOOK(o);

    if (o->op_flags & OPf_KIDS) {
        register OP *kid, *nextkid;
	for (kid = cUNOPo->op_first; kid; kid = nextkid) {
	    nextkid = kid->op_sibling; /* Get before next freeing kid */
	    op_free(kid);
	}
    }

#ifdef PERL_DEBUG_READONLY_OPS
    Slab_to_rw(o);
#endif

    /* COP* is not cleared by op_clear() so that we may track line
     * numbers etc even after null() */
    if (type == OP_NEXTSTATE || type == OP_DBSTATE
	    || (type == OP_NULL /* the COP might have been null'ed */
		&& ((OPCODE)o->op_targ == OP_NEXTSTATE
		    || (OPCODE)o->op_targ == OP_DBSTATE))) {
	cop_free((COP*)o);
    }

    if (type == OP_NULL)
	type = (OPCODE)o->op_targ;

    op_clear(o);
    if (o->op_latefree) {
	o->op_latefreed = 1;
	return;
    }
  do_free:
    FreeOp(o);
#ifdef DEBUG_LEAKING_SCALARS
    if (PL_op == o)
	PL_op = NULL;
#endif
}

void
Perl_op_clear(pTHX_ OP *o)
{

    dVAR;

    PERL_ARGS_ASSERT_OP_CLEAR;

#ifdef PERL_MAD
    /* if (o->op_madprop && o->op_madprop->mad_next)
       abort(); */
    /* FIXME for MAD - if I uncomment these two lines t/op/pack.t fails with
       "modification of a read only value" for a reason I can't fathom why.
       It's the "" stringification of $_, where $_ was set to '' in a foreach
       loop, but it defies simplification into a small test case.
       However, commenting them out has caused ext/List/Util/t/weak.t to fail
       the last test.  */
    /*
      mad_free(o->op_madprop);
      o->op_madprop = 0;
    */
#endif    

 retry:
    switch (o->op_type) {
    case OP_NULL:	/* Was holding old type, if any. */
	if (PL_madskills && o->op_targ != OP_NULL) {
	    o->op_type = (Optype)o->op_targ;
	    o->op_targ = 0;
	    goto retry;
	}
    case OP_ENTERTRY:
    case OP_ENTEREVAL:	/* Was holding hints. */
	o->op_targ = 0;
	break;
    default:
	if (!(o->op_flags & OPf_REF)
	    || (PL_check[o->op_type] != MEMBER_TO_FPTR(Perl_ck_ftst)))
	    break;
	/* FALL THROUGH */
    case OP_GVSV:
    case OP_GV:
    case OP_AELEMFAST:
	if (! (o->op_type == OP_AELEMFAST && o->op_flags & OPf_SPECIAL)) {
	    /* not an OP_PADAV replacement */
	    GV *gv = (o->op_type == OP_GV || o->op_type == OP_GVSV)
#ifdef USE_ITHREADS
			&& PL_curpad
#endif
			? cGVOPo_gv : NULL;
	    /* It's possible during global destruction that the GV is freed
	       before the optree. Whilst the SvREFCNT_inc is happy to bump from
	       0 to 1 on a freed SV, the corresponding SvREFCNT_dec from 1 to 0
	       will trigger an assertion failure, because the entry to sv_clear
	       checks that the scalar is not already freed.  A check of for
	       !SvIS_FREED(gv) turns out to be invalid, because during global
	       destruction the reference count can be forced down to zero
	       (with SVf_BREAK set).  In which case raising to 1 and then
	       dropping to 0 triggers cleanup before it should happen.  I
	       *think* that this might actually be a general, systematic,
	       weakness of the whole idea of SVf_BREAK, in that code *is*
	       allowed to raise and lower references during global destruction,
	       so any *valid* code that happens to do this during global
	       destruction might well trigger premature cleanup.  */
	    bool still_valid = gv && SvREFCNT(gv);

	    if (still_valid)
		SvREFCNT_inc_simple_void(gv);
#ifdef USE_ITHREADS
	    if (cPADOPo->op_padix > 0) {
		/* No GvIN_PAD_off(cGVOPo_gv) here, because other references
		 * may still exist on the pad */
		pad_swipe(cPADOPo->op_padix, TRUE);
		cPADOPo->op_padix = 0;
	    }
#else
	    SvREFCNT_dec(cSVOPo->op_sv);
	    cSVOPo->op_sv = NULL;
#endif
	    if (still_valid) {
		int try_downgrade = SvREFCNT(gv) == 2;
		SvREFCNT_dec(gv);
		if (try_downgrade)
		    gv_try_downgrade(gv);
	    }
	}
	break;
    case OP_METHOD_NAMED:
    case OP_CONST:
    case OP_HINTSEVAL:
	SvREFCNT_dec(cSVOPo->op_sv);
	cSVOPo->op_sv = NULL;
#ifdef USE_ITHREADS
	/** Bug #15654
	  Even if op_clear does a pad_free for the target of the op,
	  pad_free doesn't actually remove the sv that exists in the pad;
	  instead it lives on. This results in that it could be reused as 
	  a target later on when the pad was reallocated.
	**/
        if(o->op_targ) {
          pad_swipe(o->op_targ,1);
          o->op_targ = 0;
        }
#endif
	break;
    case OP_GOTO:
    case OP_NEXT:
    case OP_LAST:
    case OP_REDO:
	if (o->op_flags & (OPf_SPECIAL|OPf_STACKED|OPf_KIDS))
	    break;
	/* FALL THROUGH */
    case OP_TRANS:
	if (o->op_private & (OPpTRANS_FROM_UTF|OPpTRANS_TO_UTF)) {
#ifdef USE_ITHREADS
	    if (cPADOPo->op_padix > 0) {
		pad_swipe(cPADOPo->op_padix, TRUE);
		cPADOPo->op_padix = 0;
	    }
#else
	    SvREFCNT_dec(cSVOPo->op_sv);
	    cSVOPo->op_sv = NULL;
#endif
	}
	else {
	    PerlMemShared_free(cPVOPo->op_pv);
	    cPVOPo->op_pv = NULL;
	}
	break;
    case OP_SUBST:
	op_free(cPMOPo->op_pmreplrootu.op_pmreplroot);
	goto clear_pmop;
    case OP_PUSHRE:
#ifdef USE_ITHREADS
        if (cPMOPo->op_pmreplrootu.op_pmtargetoff) {
	    /* No GvIN_PAD_off here, because other references may still
	     * exist on the pad */
	    pad_swipe(cPMOPo->op_pmreplrootu.op_pmtargetoff, TRUE);
	}
#else
	SvREFCNT_dec(MUTABLE_SV(cPMOPo->op_pmreplrootu.op_pmtargetgv));
#endif
	/* FALL THROUGH */
    case OP_MATCH:
    case OP_QR:
clear_pmop:
	forget_pmop(cPMOPo, 1);
	cPMOPo->op_pmreplrootu.op_pmreplroot = NULL;
        /* we use the same protection as the "SAFE" version of the PM_ macros
         * here since sv_clean_all might release some PMOPs
         * after PL_regex_padav has been cleared
         * and the clearing of PL_regex_padav needs to
         * happen before sv_clean_all
         */
#ifdef USE_ITHREADS
	if(PL_regex_pad) {        /* We could be in destruction */
	    const IV offset = (cPMOPo)->op_pmoffset;
	    ReREFCNT_dec(PM_GETRE(cPMOPo));
	    PL_regex_pad[offset] = &PL_sv_undef;
            sv_catpvn_nomg(PL_regex_pad[0], (const char *)&offset,
			   sizeof(offset));
        }
#else
	ReREFCNT_dec(PM_GETRE(cPMOPo));
	PM_SETRE(cPMOPo, NULL);
#endif

	break;
    }

    if (o->op_targ > 0) {
	pad_free(o->op_targ);
	o->op_targ = 0;
    }
}

STATIC void
S_cop_free(pTHX_ COP* cop)
{
    PERL_ARGS_ASSERT_COP_FREE;

    CopFILE_free(cop);
    CopSTASH_free(cop);
    if (! specialWARN(cop->cop_warnings))
	PerlMemShared_free(cop->cop_warnings);
    Perl_refcounted_he_free(aTHX_ cop->cop_hints_hash);
}

STATIC void
S_forget_pmop(pTHX_ PMOP *const o
#ifdef USE_ITHREADS
	      , U32 flags
#endif
	      )
{
    HV * const pmstash = PmopSTASH(o);

    PERL_ARGS_ASSERT_FORGET_PMOP;

    if (pmstash && !SvIS_FREED(pmstash)) {
	MAGIC * const mg = mg_find((const SV *)pmstash, PERL_MAGIC_symtab);
	if (mg) {
	    PMOP **const array = (PMOP**) mg->mg_ptr;
	    U32 count = mg->mg_len / sizeof(PMOP**);
	    U32 i = count;

	    while (i--) {
		if (array[i] == o) {
		    /* Found it. Move the entry at the end to overwrite it.  */
		    array[i] = array[--count];
		    mg->mg_len = count * sizeof(PMOP**);
		    /* Could realloc smaller at this point always, but probably
		       not worth it. Probably worth free()ing if we're the
		       last.  */
		    if(!count) {
			Safefree(mg->mg_ptr);
			mg->mg_ptr = NULL;
		    }
		    break;
		}
	    }
	}
    }
    if (PL_curpm == o) 
	PL_curpm = NULL;
#ifdef USE_ITHREADS
    if (flags)
	PmopSTASH_free(o);
#endif
}

STATIC void
S_find_and_forget_pmops(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_FIND_AND_FORGET_PMOPS;

    if (o->op_flags & OPf_KIDS) {
        OP *kid = cUNOPo->op_first;
	while (kid) {
	    switch (kid->op_type) {
	    case OP_SUBST:
	    case OP_PUSHRE:
	    case OP_MATCH:
	    case OP_QR:
		forget_pmop((PMOP*)kid, 0);
	    }
	    find_and_forget_pmops(kid);
	    kid = kid->op_sibling;
	}
    }
}

void
Perl_op_null(pTHX_ OP *o)
{
    dVAR;

    PERL_ARGS_ASSERT_OP_NULL;

    if (o->op_type == OP_NULL)
	return;
    if (!PL_madskills)
	op_clear(o);
    o->op_targ = o->op_type;
    o->op_type = OP_NULL;
}

void
Perl_op_refcnt_lock(pTHX)
{
    dVAR;
    PERL_UNUSED_CONTEXT;
    OP_REFCNT_LOCK;
}

void
Perl_op_refcnt_unlock(pTHX)
{
    dVAR;
    PERL_UNUSED_CONTEXT;
    OP_REFCNT_UNLOCK;
}

/* Contextualizers */

static OP *
S_scalarkids(pTHX_ OP *o)
{
    if (o && o->op_flags & OPf_KIDS) {
	OP *kid;
	for (kid = cLISTOPo->op_first; kid; kid = kid->op_sibling)
	    scalar(kid);
    }
    return o;
}

STATIC OP *
S_scalarboolean(pTHX_ OP *o)
{
    dVAR;

    PERL_ARGS_ASSERT_SCALARBOOLEAN;

    if (o->op_type == OP_SASSIGN && cBINOPo->op_first->op_type == OP_CONST) {
	if (ckWARN(WARN_SYNTAX)) {
	    const line_t oldline = CopLINE(PL_curcop);

	    if (PL_parser && PL_parser->copline != NOLINE)
		CopLINE_set(PL_curcop, PL_parser->copline);
	    Perl_warner(aTHX_ packWARN(WARN_SYNTAX), "Found = in conditional, should be ==");
	    CopLINE_set(PL_curcop, oldline);
	}
    }
    return scalar(o);
}

OP *
Perl_scalar(pTHX_ OP *o)
{
    dVAR;
    OP *kid;

    /* assumes no premature commitment */
    if (!o || (PL_parser && PL_parser->error_count)
	 || (o->op_flags & OPf_WANT)
	 || o->op_type == OP_RETURN)
    {
	return o;
    }

    assert(!o->op_context_known);
    o->op_flags = (o->op_flags & ~OPf_WANT) | OPf_WANT_SCALAR;
    o->op_context_known = TRUE;

    switch (o->op_type) {
    case OP_REPEAT:
	scalar(cBINOPo->op_first);
	break;
    case OP_OR:
    case OP_AND:
    case OP_COND_EXPR:
	for (kid = cUNOPo->op_first->op_sibling; kid; kid = kid->op_sibling)
	    scalar(kid);
	break;
	/* FALL THROUGH */
    case OP_SPLIT:
    case OP_MATCH:
    case OP_QR:
    case OP_SUBST:
    case OP_NULL:
    default:
	if (o->op_flags & OPf_KIDS) {
	    for (kid = cUNOPo->op_first; kid; kid = kid->op_sibling)
		scalar(kid);
	}
	break;
    case OP_LEAVE:
    case OP_LEAVETRY:
	kid = cLISTOPo->op_first;
	scalar(kid);
	kid = kid->op_sibling;
    do_kids:
	while (kid) {
	    OP *sib = kid->op_sibling;
	    if (sib && kid->op_type != OP_LEAVEWHEN) {
		if (sib->op_type == OP_BREAK && sib->op_flags & OPf_SPECIAL) {
		    scalar(kid);
		    scalarvoid(sib);
		    break;
		} else
		    scalarvoid(kid);
	    } else
		scalar(kid);
	    kid = sib;
	}
	PL_curcop = &PL_compiling;
	break;
    case OP_SCOPE:
    case OP_LINESEQ:
    case OP_LIST:
	kid = cLISTOPo->op_first;
	goto do_kids;
    case OP_SORT:
	Perl_ck_warner(aTHX_ packWARN(WARN_VOID), "Useless use of sort in scalar context");
	break;
    case OP_ENTERSUB:
	break;
    }
    return o;
}

OP *
Perl_scalarvoid(pTHX_ OP *o)
{
    dVAR;
    OP *kid;
    const char* useless = NULL;
    SV* sv;
    U8 want;

    PERL_ARGS_ASSERT_SCALARVOID;

    /* trailing mad null ops don't count as "there" for void processing */
    if (PL_madskills &&
    	o->op_type != OP_NULL &&
	o->op_sibling &&
	o->op_sibling->op_type == OP_NULL)
    {
	OP *sib;
	for (sib = o->op_sibling;
		sib && sib->op_type == OP_NULL;
		sib = sib->op_sibling) ;
	
	if (!sib)
	    return o;
    }

    if (o->op_type == OP_NEXTSTATE
	|| o->op_type == OP_DBSTATE
	|| (o->op_type == OP_NULL && (o->op_targ == OP_NEXTSTATE
				      || o->op_targ == OP_DBSTATE)))
	PL_curcop = (COP*)o;		/* for warning below */

    /* assumes no premature commitment */
    want = o->op_flags & OPf_WANT;
    if ((want && want != OPf_WANT_SCALAR)
	 || (PL_parser && PL_parser->error_count)
	 || o->op_type == OP_RETURN || o->op_type == OP_REQUIRE || o->op_type == OP_LEAVEWHEN)
    {
	return o;
    }

    if ((o->op_private & OPpTARGET_MY)
	&& (PL_opargs[o->op_type] & OA_TARGLEX))/* OPp share the meaning */
    {
	return scalar(o);			/* As if inside SASSIGN */
    }

    assert((!o->op_context_known) || want == OPf_WANT_SCALAR);
    o->op_context_known = TRUE;
    o->op_flags = (o->op_flags & ~OPf_WANT) | OPf_WANT_VOID;

    switch (o->op_type) {
    default:
	if (!(PL_opargs[o->op_type] & OA_FOLDCONST))
	    break;
	/* FALL THROUGH */
    case OP_REPEAT:
	if (o->op_flags & OPf_STACKED)
	    break;
	goto func_ops;
    case OP_SUBSTR:
	if (o->op_private == 4)
	    break;
	/* FALL THROUGH */
    case OP_GVSV:
    case OP_WANTARRAY:
    case OP_GV:
    case OP_SMARTMATCH:
    case OP_PADSV:
    case OP_PADAV:
    case OP_PADHV:
    case OP_PADANY:
    case OP_AV2ARYLEN:
    case OP_REF:
    case OP_REFGEN:
    case OP_SREFGEN:
    case OP_DEFINED:
    case OP_HEX:
    case OP_OCT:
    case OP_LENGTH:
    case OP_VEC:
    case OP_INDEX:
    case OP_RINDEX:
    case OP_SPRINTF:
    case OP_AELEM:
    case OP_AELEMFAST:
    case OP_ASLICE:
    case OP_HELEM:
    case OP_HSLICE:
    case OP_UNPACK:
    case OP_PACK:
    case OP_JOIN:
    case OP_LSLICE:
    case OP_ANONLIST:
    case OP_ANONHASH:
    case OP_SORT:
    case OP_REVERSE:
    case OP_RANGE:
    case OP_FLIP:
    case OP_FLOP:
    case OP_CALLER:
    case OP_FILENO:
    case OP_EOF:
    case OP_TELL:
    case OP_GETSOCKNAME:
    case OP_GETPEERNAME:
    case OP_READLINK:
    case OP_TELLDIR:
    case OP_GETPPID:
    case OP_GETPGRP:
    case OP_GETPRIORITY:
    case OP_TIME:
    case OP_TMS:
    case OP_LOCALTIME:
    case OP_GMTIME:
    case OP_GHBYNAME:
    case OP_GHBYADDR:
    case OP_GHOSTENT:
    case OP_GNBYNAME:
    case OP_GNBYADDR:
    case OP_GNETENT:
    case OP_GPBYNAME:
    case OP_GPBYNUMBER:
    case OP_GPROTOENT:
    case OP_GSBYNAME:
    case OP_GSBYPORT:
    case OP_GSERVENT:
    case OP_GPWNAM:
    case OP_GPWUID:
    case OP_GGRNAM:
    case OP_GGRGID:
    case OP_GETLOGIN:
    case OP_PROTOTYPE:
      func_ops:
	if (!(o->op_private & (OPpLVAL_INTRO|OPpOUR_INTRO)))
	    /* Otherwise it's "Useless use of grep iterator" */
	    useless = OP_DESC(o);
	break;

    case OP_SPLIT:
	kid = cLISTOPo->op_first;
	if (kid && kid->op_type == OP_PUSHRE
#ifdef USE_ITHREADS
		&& !((PMOP*)kid)->op_pmreplrootu.op_pmtargetoff)
#else
		&& !((PMOP*)kid)->op_pmreplrootu.op_pmtargetgv)
#endif
	    useless = OP_DESC(o);
	break;

    case OP_NOT:
       kid = cUNOPo->op_first;
       if (kid->op_type != OP_MATCH && kid->op_type != OP_SUBST &&
           kid->op_type != OP_TRANS) {
	        goto func_ops;
       }
       useless = "negative pattern binding (!~)";
       break;

    case OP_SUBST:
	if (cPMOPo->op_pmflags & PMf_NONDESTRUCT)
	    useless = "Non-destructive substitution (s///r)";
	break;

    case OP_RV2GV:
    case OP_RV2SV:
    case OP_RV2AV:
    case OP_RV2HV:
	if (!(o->op_private & (OPpLVAL_INTRO|OPpOUR_INTRO)) &&
		(!o->op_sibling || o->op_sibling->op_type != OP_READLINE))
	    useless = "a variable";
	break;

    case OP_CONST:
	sv = cSVOPo_sv;
	if (cSVOPo->op_private & OPpCONST_STRICT)
	    no_bareword_allowed(o);
	else {
	    if (ckWARN(WARN_VOID)) {
		if (SvOK(sv)) {
		    SV* msv = sv_2mortal(Perl_newSVpvf(aTHX_
				"a constant (%"SVf")", sv));
		    useless = SvPV_nolen(msv);
		}
		else
		    useless = "a constant (undef)";
		if (o->op_private & OPpCONST_ARYBASE)
		    useless = NULL;
		/* don't warn on optimised away booleans, eg 
		 * use constant Foo, 5; Foo || print; */
		if (cSVOPo->op_private & OPpCONST_SHORTCIRCUIT)
		    useless = NULL;
		/* the constants 0 and 1 are permitted as they are
		   conventionally used as dummies in constructs like
		        1 while some_condition_with_side_effects;  */
		else if (SvNIOK(sv) && (SvNV(sv) == 0.0 || SvNV(sv) == 1.0))
		    useless = NULL;
		else if (SvPOK(sv)) {
                  /* perl4's way of mixing documentation and code
                     (before the invention of POD) was based on a
                     trick to mix nroff and perl code. The trick was
                     built upon these three nroff macros being used in
                     void context. The pink camel has the details in
                     the script wrapman near page 319. */
		    const char * const maybe_macro = SvPVX_const(sv);
		    if (strnEQ(maybe_macro, "di", 2) ||
			strnEQ(maybe_macro, "ds", 2) ||
			strnEQ(maybe_macro, "ig", 2))
			    useless = NULL;
		}
	    }
	}
	break;

    case OP_POSTINC:
	o->op_type = OP_PREINC;		/* pre-increment is faster */
	break;

    case OP_POSTDEC:
	o->op_type = OP_PREDEC;		/* pre-decrement is faster */
	break;

    case OP_I_POSTINC:
	o->op_type = OP_I_PREINC;	/* pre-increment is faster */
	break;

    case OP_I_POSTDEC:
	o->op_type = OP_I_PREDEC;	/* pre-decrement is faster */
	break;


    case OP_SASSIGN:
    {
	UNOP* refgen;
	LISTOP* exlist;
	UNOP* rv2cv;
	OP* rv2gv;

	rv2gv = ((BINOP *)o)->op_last;
	if (!rv2gv || rv2gv->op_type != OP_RV2GV)
	    break;

	refgen = (UNOP *)((BINOP *)o)->op_first;

	if (!refgen || refgen->op_type != OP_REFGEN)
	    break;

	exlist = (LISTOP *)refgen->op_first;
	if (!exlist || exlist->op_type != OP_LIST)
	    break;

	rv2cv = (UNOP*)exlist->op_first;

	if (rv2cv->op_type != OP_RV2CV)
	    break;

	assert ((rv2gv->op_private & OPpDONT_INIT_GV) == 0);
	assert ((o->op_private & OPpASSIGN_CV_TO_GV) == 0);
	assert ((rv2cv->op_private & OPpMAY_RETURN_CONSTANT) == 0);
	
	o->op_private |= OPpASSIGN_CV_TO_GV;
	rv2gv->op_private |= OPpDONT_INIT_GV;
	rv2cv->op_private |= OPpMAY_RETURN_CONSTANT;
	break;
    }

    case OP_OR:
    case OP_AND:
	kid = cLOGOPo->op_first;
	if (kid->op_type == OP_NOT
	    && (kid->op_flags & OPf_KIDS)
	    && !PL_madskills) {
	    if (o->op_type == OP_AND) {
		o->op_type = OP_OR;
	    } else {
		o->op_type = OP_AND;
	    }
	    op_null(kid);
	}

    case OP_DOR:
    case OP_COND_EXPR:
    case OP_ENTERGIVEN:
    case OP_WHILE_AND:
	for (kid = cUNOPo->op_first->op_sibling; kid; kid = kid->op_sibling)
	    scalarvoid(kid);
	break;
    case OP_ENTERWHEN:
	if (o->op_flags & OPf_SPECIAL)
	    scalarvoid(cUNOPo->op_first);
	for (kid = cUNOPo->op_first->op_sibling; kid; kid = kid->op_sibling)
	    scalarvoid(kid);
	break;

    case OP_NULL:
	if (o->op_flags & OPf_STACKED)
	    break;
	/* FALL THROUGH */
    case OP_NEXTSTATE:
    case OP_DBSTATE:
    case OP_ENTERTRY:
    case OP_ENTER:
	if (!(o->op_flags & OPf_KIDS))
	    break;
	/* FALL THROUGH */
    case OP_SCOPE:
    case OP_LEAVE:
    case OP_LEAVETRY:
    case OP_LEAVELOOP:
    case OP_LINESEQ:
    case OP_LIST:
    case OP_LEAVEGIVEN:
    case OP_LEAVEWHEN:
    case OP_LEAVEEVAL:
	for (kid = cLISTOPo->op_first; kid; kid = kid->op_sibling)
	    scalarvoid(kid);
	break;
    case OP_ENTERLOOP:
	for (kid = cLISTOPo->op_first->op_sibling; kid; kid = kid->op_sibling)
	    scalarvoid(kid);
	break;
    case OP_FOREACH:
	for (kid = cLISTOPo->op_first->op_sibling->op_sibling; kid; kid = kid->op_sibling)
	    scalarvoid(kid);
	break;
    case OP_SCALAR:
	return scalar(o);
    }
    if (useless)
	Perl_ck_warner(aTHX_ packWARN(WARN_VOID), "Useless use of %s in void context", useless);
    return o;
}

static OP *
S_listkids(pTHX_ OP *o)
{
    if (o && o->op_flags & OPf_KIDS) {
        OP *kid;
	for (kid = cLISTOPo->op_first; kid; kid = kid->op_sibling)
	    list(kid);
    }
    return o;
}

OP *
Perl_list(pTHX_ OP *o)
{
    dVAR;
    OP *kid;

    /* assumes no premature commitment */
    if (!o || (o->op_flags & OPf_WANT)
	 || (PL_parser && PL_parser->error_count)
	 || o->op_type == OP_RETURN)
    {
	return o;
    }

    if ((o->op_private & OPpTARGET_MY)
	&& (PL_opargs[o->op_type] & OA_TARGLEX))/* OPp share the meaning */
    {
	return o;				/* As if inside SASSIGN */
    }

    assert(!o->op_context_known);
    o->op_flags = (o->op_flags & ~OPf_WANT) | OPf_WANT_LIST;
    o->op_context_known = TRUE;

    switch (o->op_type) {
    case OP_FLOP:
    case OP_REPEAT:
	list(cBINOPo->op_first);
	break;
    case OP_OR:
    case OP_AND:
    case OP_COND_EXPR:
	for (kid = cUNOPo->op_first->op_sibling; kid; kid = kid->op_sibling)
	    list(kid);
	break;
    default:
    case OP_MATCH:
    case OP_QR:
    case OP_SUBST:
    case OP_NULL:
	if (!(o->op_flags & OPf_KIDS))
	    break;
	if (cUNOPo->op_first->op_type == OP_FLOP) {
	    list(cBINOPo->op_first);
	    return o;
	}
    case OP_LIST:
	listkids(o);
	break;
    case OP_LEAVE:
    case OP_LEAVETRY:
	kid = cLISTOPo->op_first;
	list(kid);
	kid = kid->op_sibling;
    do_kids:
	while (kid) {
	    OP *sib = kid->op_sibling;
	    if (sib && kid->op_type != OP_LEAVEWHEN) {
		if (sib->op_type == OP_BREAK && sib->op_flags & OPf_SPECIAL) {
		    list(kid);
		    scalarvoid(sib);
		    break;
		} else
		    scalarvoid(kid);
	    } else
		list(kid);
	    kid = sib;
	}
	PL_curcop = &PL_compiling;
	break;
    case OP_SCOPE:
    case OP_LINESEQ:
	kid = cLISTOPo->op_first;
	goto do_kids;
    }
    return o;
}

static OP *
S_scalarseq(pTHX_ OP *o)
{
    dVAR;
    if (o) {
	const OPCODE type = o->op_type;

	if (type == OP_LINESEQ || type == OP_SCOPE ||
	    type == OP_LEAVE || type == OP_LEAVETRY)
	{
            OP *kid;
	    for (kid = cLISTOPo->op_first; kid; kid = kid->op_sibling) {
		if (kid->op_sibling) {
		    scalarvoid(kid);
		}
	    }
	    PL_curcop = &PL_compiling;
	}
	o->op_flags &= ~OPf_PARENS;
	if (PL_hints & HINT_BLOCK_SCOPE)
	    o->op_flags |= OPf_PARENS;
    }
    else
	o = newOP(OP_STUB, 0);
    return o;
}

/* Fixes the context to unknown */

static OP *
S_unknown(pTHX_ OP *o)
{
    dVAR;

    /* assumes no premature commitment */
    if (!o || (o->op_flags & OPf_WANT) || o->op_type == OP_RETURN)
    {
	return o;
    }

    assert(!o->op_context_known);
    o->op_context_known = TRUE;

    if (o->op_flags & OPf_KIDS) {
	OP *kid;
	for (kid = cUNOPo->op_first; kid; kid = kid->op_sibling)
	    S_unknown(kid);
    }

    return o;
}

STATIC OP *
S_modkids(pTHX_ OP *o, I32 type)
{
    if (o && o->op_flags & OPf_KIDS) {
        OP *kid;
	for (kid = cLISTOPo->op_first; kid; kid = kid->op_sibling)
	    mod(kid, type);
    }
    return o;
}

static void
S_finished_op_check(pTHX_ OP* o)
{
    assert(o->op_context_known
	|| o->op_type == OP_NULL
	|| o->op_type == OP_LIST
	|| o->op_type == OP_STUB
	|| o->op_type == OP_NOTHING
	|| o->op_type == OP_ENTER
	|| o->op_type == OP_METHOD
	|| o->op_type == OP_METHOD_NAMED
	|| o->op_type == OP_REFGEN
	|| o->op_type == OP_CONST
	|| o->op_type == OP_PADSV
	|| o->op_type == OP_RV2CV
	); /* All ops must have a context */
    
    switch (o->op_type) {
    case OP_NEXTSTATE:
    case OP_DBSTATE:
	PL_curcop = ((COP*)o);		/* for warnings */
	break;
    case OP_EXEC:
	if (ckWARN(WARN_SYNTAX))
	    {
	    	if (o->op_sibling && o->op_sibling->op_type == OP_NEXTSTATE &&
		    o->op_sibling->op_sibling) {
	    	    const OPCODE type = o->op_sibling->op_sibling->op_type;
	    	    if (type != OP_EXIT && type != OP_WARN && type != OP_DIE) {
	    		const line_t oldline = CopLINE(PL_curcop);
	    		CopLINE_set(PL_curcop, CopLINE((COP*)o->op_sibling));
	    		Perl_warner(aTHX_ packWARN(WARN_EXEC),
	    			    "Statement unlikely to be reached");
	    		Perl_warner(aTHX_ packWARN(WARN_EXEC),
	    			    "\t(Maybe you meant system() when you said exec()?)\n");
	    		CopLINE_set(PL_curcop, oldline);
	    	    }
		}
	    }
	break;

    case OP_GV:
	if ((o->op_private & OPpEARLY_CV) && ckWARN(WARN_PROTOTYPE)) {
	    GV * const gv = cGVOPo_gv;
	    if (SvTYPE(gv) == SVt_PVGV && GvCV(gv) && SvPVX_const(GvCV(gv))) {
		/* XXX could check prototype here instead of just carping */
		SV * const sv = sv_newmortal();
		gv_efullname3(sv, gv, NULL);
		Perl_warner(aTHX_ packWARN(WARN_PROTOTYPE),
		    "%"SVf"() called too early to check prototype",
		    SVfARG(sv));
	    }
	}
	break;

    case OP_CONST:
	if (o->op_private & OPpCONST_STRICT)
	    no_bareword_allowed(o);
	/* FALL THROUGH */
#ifdef USE_ITHREADS
    case OP_HINTSEVAL:
    case OP_METHOD_NAMED:
	/* Relocate sv to the pad for thread safety.
	 * Despite being a "constant", the SV is written to,
	 * for reference counts, sv_upgrade() etc. */
	if (cSVOP->op_sv) {
	    const PADOFFSET ix = pad_alloc(OP_CONST, SVs_PADTMP);
	    if (o->op_type != OP_METHOD_NAMED && SvPADTMP(cSVOPo->op_sv)) {
		/* If op_sv is already a PADTMP then it is being used by
		 * some pad, so make a copy. */
		sv_setsv(PAD_SVl(ix),cSVOPo->op_sv);
		SvREADONLY_on(PAD_SVl(ix));
		SvREFCNT_dec(cSVOPo->op_sv);
	    }
	    else if (o->op_type != OP_METHOD_NAMED
		&& cSVOPo->op_sv == &PL_sv_undef) {
		/* PL_sv_undef is hack - it's unsafe to store it in the
		   AV that is the pad, because av_fetch treats values of
		   PL_sv_undef as a "free" AV entry and will merrily
		   replace them with a new SV, causing pad_alloc to think
		   that this pad slot is free. (When, clearly, it is not)
		*/
		SvOK_off(PAD_SVl(ix));
		SvPADTMP_on(PAD_SVl(ix));
		SvREADONLY_on(PAD_SVl(ix));
	    }
	    else {
		SvREFCNT_dec(PAD_SVl(ix));
		SvPADTMP_on(cSVOPo->op_sv);
		PAD_SETSV(ix, cSVOPo->op_sv);
		/* XXX I don't know how this isn't readonly already. */
		SvREADONLY_on(PAD_SVl(ix));
	    }
	    cSVOPo->op_sv = NULL;
	    o->op_targ = ix;
	}
#endif
	break;

    case OP_HELEM:
    {
	SV* keysv;
	SV* lexname;
	UNOP* rop;
	GV** fields;
	const char* key;
	STRLEN keylen;

	if (((BINOP*)o)->op_last->op_type != OP_CONST)
	    break;
	keysv = cSVOPx_sv(cBINOPx(o)->op_last);

	if ((o->op_private & (OPpLVAL_INTRO)))
	    break;

	rop = (UNOP*)((BINOP*)o)->op_first;
	if (rop->op_type != OP_RV2HV || rop->op_first->op_type != OP_PADSV)
	    break;
	lexname = *av_fetch(PL_comppad_name, rop->op_first->op_targ, TRUE);
	if (!SvPAD_TYPED(lexname))
	    break;
	fields = (GV**)hv_fetchs(SvSTASH(lexname), "FIELDS", FALSE);
	if (!fields || !GvHV(*fields))
	    break;
	key = SvPV_const(keysv, keylen);
	if (!hv_fetch(GvHV(*fields), key,
		SvUTF8(keysv) ? -(I32)keylen : (I32)keylen, FALSE))
	    {
		Perl_croak(aTHX_ "No such class field \"%s\" " 
			   "in variable %s of type %s", 
		      key, SvPV_nolen_const(lexname), HvNAME_get(SvSTASH(lexname)));
	    }
	break;
    }

    case OP_HSLICE: {
	UNOP *rop;
	SV *lexname;
	GV **fields;
	SV **svp;
	const char *key;
	STRLEN keylen;
	SVOP *first_key_op, *key_op;

	if ((o->op_private & (OPpLVAL_INTRO))
	    || ((LISTOP*)o)->op_first->op_type != OP_LIST)
	    /* hmmm, no optimization if list contains only one key. */
	    break;
	rop = (UNOP*)((LISTOP*)o)->op_last;
	if (rop->op_type != OP_RV2HV)
	    break;
	if (rop->op_first->op_type == OP_PADSV)
	    /* @$hash{qw(keys here)} */
	    rop = (UNOP*)rop->op_first;
	else {
	    /* @{$hash}{qw(keys here)} */
	    if (rop->op_first->op_type == OP_SCOPE 
		&& cLISTOPx(rop->op_first)->op_last->op_type == OP_PADSV)
		{
		    rop = (UNOP*)cLISTOPx(rop->op_first)->op_last;
		}
	    else
		break;
	}
		    
	lexname = *av_fetch(PL_comppad_name, rop->op_targ, TRUE);
	if (!SvPAD_TYPED(lexname))
	    break;
	fields = (GV**)hv_fetchs(SvSTASH(lexname), "FIELDS", FALSE);
	if (!fields || !GvHV(*fields))
	    break;
	first_key_op = (SVOP*)((LISTOP*)((LISTOP*)o)->op_first)
	    ->op_first;
	for (key_op = first_key_op; key_op;
	     key_op = (SVOP*)key_op->op_sibling) {
	    if (key_op->op_type != OP_CONST)
		continue;
	    svp = cSVOPx_svp(key_op);
	    key = SvPV_const(*svp, keylen);
	    if (!hv_fetch(GvHV(*fields), key, 
		    SvUTF8(*svp) ? -(I32)keylen : (I32)keylen, FALSE))
		{
		    Perl_croak(aTHX_ "No such class field \"%s\" "
			"in variable %s of type %s",
			key, SvPV_nolen(lexname), HvNAME_get(SvSTASH(lexname)));
		}
	}
	break;
    }

    default:
	break;
    }

    if (o->op_flags & OPf_KIDS) {
	OP *kid;
	for (kid = cUNOPo->op_first; kid; kid = kid->op_sibling)
	    S_finished_op_check(kid);
    }
}

void
Perl_finish_optree(pTHX_ OP* o)
{
    COP* oldcop = PL_curcop;

    PERL_ARGS_ASSERT_FINISH_OPTREE;

    S_finished_op_check(o);
    
    PL_curcop = oldcop;
}

/* Propagate lvalue ("modifiable") context to an op and its children.
 * 'type' represents the context type, roughly based on the type of op that
 * would do the modifying, although local() is represented by OP_NULL.
 * It's responsible for detecting things that can't be modified,  flag
 * things that need to behave specially in an lvalue context (e.g., "$$x = 5"
 * might have to vivify a reference in $x), and so on.
 *
 * For example, "$a+1 = 2" would cause mod() to be called with o being
 * OP_ADD and type being OP_SASSIGN, and would output an error.
 */

OP *
Perl_mod(pTHX_ OP *o, I32 type)
{
    dVAR;
    OP *kid;
    /* -1 = error on localize, 0 = ignore localize, 1 = ok to localize */
    int localize = -1;

    if (!o || (PL_parser && PL_parser->error_count))
	return o;

    if ((o->op_private & OPpTARGET_MY)
	&& (PL_opargs[o->op_type] & OA_TARGLEX))/* OPp share the meaning */
    {
	return o;
    }

    switch (o->op_type) {
    case OP_UNDEF:
	localize = 0;
	PL_modcount++;
	return o;
    case OP_CONST:
	if (!(o->op_private & OPpCONST_ARYBASE))
	    goto nomod;
	localize = 0;
	if (PL_eval_root && PL_eval_root->op_type == OP_CONST) {
	    CopARYBASE_set(&PL_compiling,
			   (I32)SvIV(cSVOPx(PL_eval_root)->op_sv));
	    PL_eval_root = 0;
	}
	else if (PL_eval_root && PL_eval_root->op_type == OP_NEGATE && cUNOPx(PL_eval_root)->op_first->op_type == OP_CONST) {
	    CopARYBASE_set(&PL_compiling,
		-(I32)SvIV(cSVOPx(cUNOPx(PL_eval_root)->op_first)->op_sv));
	    PL_eval_root = 0;
	}
	else if (!type) {
	    SAVECOPARYBASE(&PL_compiling);
	    CopARYBASE_set(&PL_compiling, 0);
	}
	else if (type == OP_REFGEN)
	    goto nomod;
	else
	    Perl_croak(aTHX_ "That use of $[ is unsupported");
	break;
    case OP_STUB:
	if ((o->op_flags & OPf_PARENS) || PL_madskills)
	    break;
	goto nomod;
    case OP_ENTERSUB:
	if ((type == OP_UNDEF || type == OP_REFGEN) &&
	    !(o->op_flags & OPf_STACKED)) {
	    o->op_type = OP_RV2CV;		/* entersub => rv2cv */
	    /* The default is to set op_private to the number of children,
	       which for a UNOP such as RV2CV is always 1. And w're using
	       the bit for a flag in RV2CV, so we need it clear.  */
	    o->op_private &= ~1;
	    break;
	}
	else if (o->op_private & OPpENTERSUB_NOMOD)
	    return o;
	else {				/* lvalue subroutine call */
	    o->op_private |= OPpLVAL_INTRO;
	    PL_modcount = RETURN_UNLIMITED_NUMBER;
	    if (type == OP_GREPSTART || type == OP_ENTERSUB || type == OP_REFGEN) {
		/* Backward compatibility mode: */
		o->op_private |= OPpENTERSUB_INARGS;
		break;
	    }
	    else {                      /* Compile-time error message: */
		OP *kid = cUNOPo->op_first;
		CV *cv;
		OP *okid;

		if (kid->op_type == OP_LIST)
		    kid = kUNOP->op_first;

		while (kid->op_sibling)
		    kid = kid->op_sibling;
		if (!(kid->op_type == OP_NULL && kid->op_targ == OP_RV2CV)) {
		    /* Indirect call */
		    if (kid->op_type == OP_METHOD_NAMED
			|| kid->op_type == OP_METHOD)
		    {
			UNOP *newop;

			NewOp(1101, newop, 1, UNOP);
			newop->op_type = OP_RV2CV;
			newop->op_first = NULL;
			kid->op_sibling = (OP*)newop;
			newop->op_private |= OPpLVAL_INTRO;
			newop->op_private &= ~1;
			newop->op_flags = OPf_WANT_SCALAR;
			newop->op_context_known = TRUE;
			break;
		    }

		    if (kid->op_type != OP_RV2CV)
			Perl_croak(aTHX_
				   "panic: unexpected lvalue entersub "
				   "entry via type/targ %ld:%"UVuf,
				   (long)kid->op_type, (UV)kid->op_targ);
		    kid->op_private |= OPpLVAL_INTRO;
		    break;	/* Postpone until runtime */
		}

		okid = kid;
		kid = kUNOP->op_first;
		if (kid->op_type == OP_NULL && kid->op_targ == OP_RV2SV)
		    kid = kUNOP->op_first;
		if (kid->op_type == OP_NULL)
		    Perl_croak(aTHX_
			       "Unexpected constant lvalue entersub "
			       "entry via type/targ %ld:%"UVuf,
			       (long)kid->op_type, (UV)kid->op_targ);
		if (kid->op_type != OP_GV) {
		    /* Restore RV2CV to check lvalueness */
		  restore_2cv:
		    okid->op_type = OP_RV2CV;
		    okid->op_targ = 0;
		    okid->op_private |= OPpLVAL_INTRO;
		    okid->op_private &= ~1;
		    break;
		}

		cv = GvCV(kGVOP_gv);
		if (!cv)
		    goto restore_2cv;
		if (CvLVALUE(cv))
		    break;
	    }
	}
	/* FALL THROUGH */
    default:
      nomod:
	/* grep, foreach, subcalls, refgen */
	if (type == OP_GREPSTART || type == OP_ENTERSUB || type == OP_REFGEN)
	    break;
	yyerror(Perl_form(aTHX_ "Can't modify %s in %s",
		     (o->op_type == OP_NULL && (o->op_flags & OPf_SPECIAL)
		      ? "do block"
		      : (o->op_type == OP_ENTERSUB
			? "non-lvalue subroutine call"
			: OP_DESC(o))),
		     type ? PL_op_desc[type] : "local"));
	return o;

    case OP_PREINC:
    case OP_PREDEC:
    case OP_POW:
    case OP_MULTIPLY:
    case OP_DIVIDE:
    case OP_MODULO:
    case OP_REPEAT:
    case OP_ADD:
    case OP_SUBTRACT:
    case OP_CONCAT:
    case OP_LEFT_SHIFT:
    case OP_RIGHT_SHIFT:
    case OP_BIT_AND:
    case OP_BIT_XOR:
    case OP_BIT_OR:
    case OP_I_MULTIPLY:
    case OP_I_DIVIDE:
    case OP_I_MODULO:
    case OP_I_ADD:
    case OP_I_SUBTRACT:
	if (!(o->op_flags & OPf_STACKED))
	    goto nomod;
	PL_modcount++;
	break;

    case OP_COND_EXPR:
	localize = 1;
	for (kid = cUNOPo->op_first->op_sibling; kid; kid = kid->op_sibling)
	    mod(kid, type);
	break;

    case OP_RV2AV:
    case OP_RV2HV:
	if (type == OP_REFGEN && o->op_flags & OPf_PARENS) {
           PL_modcount = RETURN_UNLIMITED_NUMBER;
	    return o;		/* Treat \(@foo) like ordinary list. */
	}
	/* FALL THROUGH */
    case OP_RV2GV:
	if (scalar_mod_type(o, type))
	    goto nomod;
	ref(cUNOPo->op_first, o->op_type);
	/* FALL THROUGH */
    case OP_ASLICE:
    case OP_HSLICE:
	if (type == OP_LEAVESUBLV)
	    o->op_private |= OPpMAYBE_LVSUB;
	localize = 1;
	/* FALL THROUGH */
    case OP_AASSIGN:
    case OP_NEXTSTATE:
    case OP_DBSTATE:
       PL_modcount = RETURN_UNLIMITED_NUMBER;
	break;
    case OP_AV2ARYLEN:
	PL_hints |= HINT_BLOCK_SCOPE;
	if (type == OP_LEAVESUBLV)
	    o->op_private |= OPpMAYBE_LVSUB;
	PL_modcount++;
	break;
    case OP_RV2SV:
	ref(cUNOPo->op_first, o->op_type);
	localize = 1;
	/* FALL THROUGH */
    case OP_GV:
	PL_hints |= HINT_BLOCK_SCOPE;
    case OP_SASSIGN:
    case OP_ANDASSIGN:
    case OP_ORASSIGN:
    case OP_DORASSIGN:
	PL_modcount++;
	break;

    case OP_AELEMFAST:
	localize = -1;
	PL_modcount++;
	break;

    case OP_PADAV:
    case OP_PADHV:
       PL_modcount = RETURN_UNLIMITED_NUMBER;
	if (type == OP_REFGEN && o->op_flags & OPf_PARENS)
	    return o;		/* Treat \(@foo) like ordinary list. */
	if (scalar_mod_type(o, type))
	    goto nomod;
	if (type == OP_LEAVESUBLV)
	    o->op_private |= OPpMAYBE_LVSUB;
	/* FALL THROUGH */
    case OP_PADSV:
	PL_modcount++;
	if (!type) /* local() */
	    Perl_croak(aTHX_ "Can't localize lexical variable %s",
		 PAD_COMPNAME_PV(o->op_targ));
	break;

    case OP_PUSHMARK:
	localize = 0;
	break;

    case OP_KEYS:
	if (type != OP_SASSIGN)
	    goto nomod;
	pad_free(o->op_targ);
	o->op_targ = pad_alloc(o->op_type, SVs_PADMY);
	assert(SvTYPE(PAD_SV(o->op_targ)) == SVt_NULL);
	break;
    case OP_SUBSTR:
	if (o->op_private == 4) /* don't allow 4 arg substr as lvalue */
	    goto nomod;
	/* FALL THROUGH */
    case OP_POS:
    case OP_VEC:
	if (type == OP_LEAVESUBLV)
	    o->op_private |= OPpMAYBE_LVSUB;
	pad_free(o->op_targ);
	o->op_targ = pad_alloc(o->op_type, SVs_PADMY);
	assert(SvTYPE(PAD_SV(o->op_targ)) == SVt_NULL);
	if (o->op_flags & OPf_KIDS)
	    mod(cBINOPo->op_first, type);
	break;

    case OP_AELEM:
    case OP_HELEM:
	ref(cBINOPo->op_first, o->op_type);
	if (type == OP_ENTERSUB &&
	     !(o->op_private & (OPpLVAL_INTRO | OPpDEREF)))
	    o->op_private |= OPpLVAL_DEFER;
	if (type == OP_LEAVESUBLV)
	    o->op_private |= OPpMAYBE_LVSUB;
	localize = 1;
	PL_modcount++;
	break;

    case OP_SCOPE:
    case OP_LEAVE:
    case OP_ENTER:
    case OP_LINESEQ:
	localize = 0;
	if (o->op_flags & OPf_KIDS)
	    mod(cLISTOPo->op_last, type);
	break;

    case OP_NULL:
	localize = 0;
	if (o->op_flags & OPf_SPECIAL)		/* do BLOCK */
	    goto nomod;
	else if (!(o->op_flags & OPf_KIDS))
	    break;
	if (o->op_targ != OP_LIST) {
	    mod(cBINOPo->op_first, type);
	    break;
	}
	/* FALL THROUGH */
    case OP_LIST:
	localize = 0;
	for (kid = cLISTOPo->op_first; kid; kid = kid->op_sibling)
	    mod(kid, type);
	break;

    case OP_RETURN:
	if (type != OP_LEAVESUBLV)
	    goto nomod;
	break; /* mod()ing was handled by ck_return() */
    }

    /* [20011101.069] File test operators interpret OPf_REF to mean that
       their argument is a filehandle; thus \stat(".") should not set
       it. AMS 20011102 */
    if (type == OP_REFGEN &&
        PL_check[o->op_type] == MEMBER_TO_FPTR(Perl_ck_ftst))
        return o;

    if (type != OP_LEAVESUBLV)
        o->op_flags |= OPf_MOD;

    if (type == OP_AASSIGN || type == OP_SASSIGN)
	o->op_flags |= OPf_SPECIAL|OPf_REF;
    else if (!type) { /* local() */
	switch (localize) {
	case 1:
	    o->op_private |= OPpLVAL_INTRO;
	    o->op_flags &= ~OPf_SPECIAL;
	    PL_hints |= HINT_BLOCK_SCOPE;
	    break;
	case 0:
	    break;
	case -1:
	    Perl_ck_warner(aTHX_ packWARN(WARN_SYNTAX),
			   "Useless localization of %s", OP_DESC(o));
	}
    }
    else if (type != OP_GREPSTART && type != OP_ENTERSUB
             && type != OP_LEAVESUBLV)
	o->op_flags |= OPf_REF;
    return o;
}

STATIC bool
S_scalar_mod_type(const OP *o, I32 type)
{
    PERL_ARGS_ASSERT_SCALAR_MOD_TYPE;

    switch (type) {
    case OP_SASSIGN:
	if (o->op_type == OP_RV2GV)
	    return FALSE;
	/* FALL THROUGH */
    case OP_PREINC:
    case OP_PREDEC:
    case OP_POSTINC:
    case OP_POSTDEC:
    case OP_I_PREINC:
    case OP_I_PREDEC:
    case OP_I_POSTINC:
    case OP_I_POSTDEC:
    case OP_POW:
    case OP_MULTIPLY:
    case OP_DIVIDE:
    case OP_MODULO:
    case OP_REPEAT:
    case OP_ADD:
    case OP_SUBTRACT:
    case OP_I_MULTIPLY:
    case OP_I_DIVIDE:
    case OP_I_MODULO:
    case OP_I_ADD:
    case OP_I_SUBTRACT:
    case OP_LEFT_SHIFT:
    case OP_RIGHT_SHIFT:
    case OP_BIT_AND:
    case OP_BIT_XOR:
    case OP_BIT_OR:
    case OP_CONCAT:
    case OP_SUBST:
    case OP_TRANS:
    case OP_READ:
    case OP_SYSREAD:
    case OP_RECV:
    case OP_ANDASSIGN:
    case OP_ORASSIGN:
    case OP_DORASSIGN:
	return TRUE;
    default:
	return FALSE;
    }
}

STATIC bool
S_is_handle_constructor(const OP *o, I32 numargs)
{
    PERL_ARGS_ASSERT_IS_HANDLE_CONSTRUCTOR;

    switch (o->op_type) {
    case OP_PIPE_OP:
    case OP_SOCKPAIR:
	if (numargs == 2)
	    return TRUE;
	/* FALL THROUGH */
    case OP_SYSOPEN:
    case OP_OPEN:
    case OP_SELECT:		/* XXX c.f. SelectSaver.pm */
    case OP_SOCKET:
    case OP_OPEN_DIR:
    case OP_ACCEPT:
	if (numargs == 1)
	    return TRUE;
	/* FALLTHROUGH */
    default:
	return FALSE;
    }
}

static OP *
S_refkids(pTHX_ OP *o, I32 type)
{
    if (o && o->op_flags & OPf_KIDS) {
        OP *kid;
	for (kid = cLISTOPo->op_first; kid; kid = kid->op_sibling)
	    ref(kid, type);
    }
    return o;
}

OP *
Perl_doref(pTHX_ OP *o, I32 type, bool set_op_ref)
{
    dVAR;
    OP *kid;

    PERL_ARGS_ASSERT_DOREF;

    if (!o || (PL_parser && PL_parser->error_count))
	return o;

    switch (o->op_type) {
    case OP_ENTERSUB:
	if ((type == OP_EXISTS || type == OP_DEFINED || type == OP_LOCK) &&
	    !(o->op_flags & OPf_STACKED)) {
	    o->op_type = OP_RV2CV;             /* entersub => rv2cv */
	    o->op_flags |= OPf_SPECIAL;
	    o->op_private &= ~1;
	}
	break;

    case OP_COND_EXPR:
	for (kid = cUNOPo->op_first->op_sibling; kid; kid = kid->op_sibling)
	    doref(kid, type, set_op_ref);
	break;
    case OP_RV2SV:
	if (type == OP_DEFINED)
	    o->op_flags |= OPf_SPECIAL;		/* don't create GV */
	doref(cUNOPo->op_first, o->op_type, set_op_ref);
	/* FALL THROUGH */
    case OP_PADSV:
	if (type == OP_RV2SV || type == OP_RV2AV || type == OP_RV2HV) {
	    o->op_private |= (type == OP_RV2AV ? OPpDEREF_AV
			      : type == OP_RV2HV ? OPpDEREF_HV
			      : OPpDEREF_SV);
	    o->op_flags |= OPf_MOD;
	}
	break;

    case OP_RV2AV:
    case OP_RV2HV:
	if (set_op_ref)
	    o->op_flags |= OPf_REF;
	/* FALL THROUGH */
    case OP_RV2GV:
	if (type == OP_DEFINED)
	    o->op_flags |= OPf_SPECIAL;		/* don't create GV */
	doref(cUNOPo->op_first, o->op_type, set_op_ref);
	break;

    case OP_PADAV:
    case OP_PADHV:
	if (set_op_ref)
	    o->op_flags |= OPf_REF;
	break;

    case OP_SCALAR:
    case OP_NULL:
	if (!(o->op_flags & OPf_KIDS))
	    break;
	doref(cBINOPo->op_first, type, set_op_ref);
	break;
    case OP_AELEM:
    case OP_HELEM:
	doref(cBINOPo->op_first, o->op_type, set_op_ref);
	if (type == OP_RV2SV || type == OP_RV2AV || type == OP_RV2HV) {
	    o->op_private |= (type == OP_RV2AV ? OPpDEREF_AV
			      : type == OP_RV2HV ? OPpDEREF_HV
			      : OPpDEREF_SV);
	    o->op_flags |= OPf_MOD;
	}
	break;

    case OP_SCOPE:
    case OP_LEAVE:
	set_op_ref = FALSE;
	/* FALL THROUGH */
    case OP_ENTER:
    case OP_LIST:
	if (!(o->op_flags & OPf_KIDS))
	    break;
	doref(cLISTOPo->op_last, type, set_op_ref);
	break;
    default:
	break;
    }
    return scalar(o);

}

STATIC OP *
S_dup_attrlist(pTHX_ OP *o)
{
    dVAR;
    OP *rop;

    PERL_ARGS_ASSERT_DUP_ATTRLIST;

    /* An attrlist is either a simple OP_CONST or an OP_LIST with kids,
     * where the first kid is OP_PUSHMARK and the remaining ones
     * are OP_CONST.  We need to push the OP_CONST values.
     */
    if (o->op_type == OP_CONST)
	rop = newSVOP(OP_CONST, o->op_flags, SvREFCNT_inc_NN(cSVOPo->op_sv));
#ifdef PERL_MAD
    else if (o->op_type == OP_NULL)
	rop = NULL;
#endif
    else {
	assert((o->op_type == OP_LIST) && (o->op_flags & OPf_KIDS));
	rop = NULL;
	for (o = cLISTOPo->op_first; o; o=o->op_sibling) {
	    if (o->op_type == OP_CONST)
		rop = append_elem(OP_LIST, rop,
				  newSVOP(OP_CONST, o->op_flags,
					  SvREFCNT_inc_NN(cSVOPo->op_sv)));
	}
    }
    return rop;
}

STATIC void
S_apply_attrs(pTHX_ HV *stash, SV *target, OP *attrs, bool for_my)
{
    dVAR;
    SV *stashsv;

    PERL_ARGS_ASSERT_APPLY_ATTRS;

    /* fake up C<use attributes $pkg,$rv,@attrs> */
    ENTER;		/* need to protect against side-effects of 'use' */
    stashsv = stash ? newSVhek(HvNAME_HEK(stash)) : &PL_sv_no;

#define ATTRSMODULE "attributes"
#define ATTRSMODULE_PM "attributes.pm"

    if (for_my) {
	/* Don't force the C<use> if we don't need it. */
	SV * const * const svp = hv_fetchs(GvHVn(PL_incgv), ATTRSMODULE_PM, FALSE);
	if (svp && *svp != &PL_sv_undef)
	    NOOP;	/* already in %INC */
	else
	    Perl_load_module(aTHX_ PERL_LOADMOD_NOIMPORT,
			     newSVpvs(ATTRSMODULE), NULL);
    }
    else {
	Perl_load_module(aTHX_ PERL_LOADMOD_IMPORT_OPS,
			 newSVpvs(ATTRSMODULE),
			 NULL,
			 prepend_elem(OP_LIST,
				      newSVOP(OP_CONST, 0, stashsv),
				      prepend_elem(OP_LIST,
						   newSVOP(OP_CONST, 0,
							   newRV(target)),
						   dup_attrlist(attrs))));
    }
    LEAVE;
}

STATIC void
S_apply_attrs_my(pTHX_ HV *stash, OP *target, OP *attrs, OP **imopsp)
{
    dVAR;
    OP *pack, *imop, *arg;
    SV *meth, *stashsv;

    PERL_ARGS_ASSERT_APPLY_ATTRS_MY;

    if (!attrs)
	return;

    assert(target->op_type == OP_PADSV ||
	   target->op_type == OP_PADHV ||
	   target->op_type == OP_PADAV);

    /* Ensure that attributes.pm is loaded. */
    apply_attrs(stash, PAD_SV(target->op_targ), attrs, TRUE);

    /* Need package name for method call. */
    pack = newSVOP(OP_CONST, 0, newSVpvs(ATTRSMODULE));

    /* Build up the real arg-list. */
    stashsv = stash ? newSVhek(HvNAME_HEK(stash)) : &PL_sv_no;

    arg = newOP(OP_PADSV, 0);
    arg->op_targ = target->op_targ;
    arg = prepend_elem(OP_LIST,
		       newSVOP(OP_CONST, 0, stashsv),
		       prepend_elem(OP_LIST,
				    newUNOP(OP_REFGEN, 0,
					    mod(arg, OP_REFGEN)),
				    dup_attrlist(attrs)));

    /* Fake up a method call to import */
    meth = newSVpvs_share("import");
    imop = convert(OP_ENTERSUB, OPf_STACKED|OPf_SPECIAL|OPf_WANT_VOID,
		   append_elem(OP_LIST,
			       prepend_elem(OP_LIST, pack, list(arg)),
			       newSVOP(OP_METHOD_NAMED, 0, meth)));
    imop->op_context_known = TRUE;
    imop->op_private |= OPpENTERSUB_NOMOD;

    /* Combine the ops. */
    *imopsp = append_elem(OP_LIST, *imopsp, imop);
}

/*
=notfor apidoc apply_attrs_string

Attempts to apply a list of attributes specified by the C<attrstr> and
C<len> arguments to the subroutine identified by the C<cv> argument which
is expected to be associated with the package identified by the C<stashpv>
argument (see L<attributes>).  It gets this wrong, though, in that it
does not correctly identify the boundaries of the individual attribute
specifications within C<attrstr>.  This is not really intended for the
public API, but has to be listed here for systems such as AIX which
need an explicit export list for symbols.  (It's called from XS code
in support of the C<ATTRS:> keyword from F<xsubpp>.)  Patches to fix it
to respect attribute syntax properly would be welcome.

=cut
*/

void
Perl_apply_attrs_string(pTHX_ const char *stashpv, CV *cv,
                        const char *attrstr, STRLEN len)
{
    OP *attrs = NULL;

    PERL_ARGS_ASSERT_APPLY_ATTRS_STRING;

    if (!len) {
        len = strlen(attrstr);
    }

    while (len) {
        for (; isSPACE(*attrstr) && len; --len, ++attrstr) ;
        if (len) {
            const char * const sstr = attrstr;
            for (; !isSPACE(*attrstr) && len; --len, ++attrstr) ;
            attrs = append_elem(OP_LIST, attrs,
                                newSVOP(OP_CONST, 0,
                                        newSVpvn(sstr, attrstr-sstr)));
        }
    }

    Perl_load_module(aTHX_ PERL_LOADMOD_IMPORT_OPS,
		     newSVpvs(ATTRSMODULE),
                     NULL, prepend_elem(OP_LIST,
				  newSVOP(OP_CONST, 0, newSVpv(stashpv,0)),
				  prepend_elem(OP_LIST,
					       newSVOP(OP_CONST, 0,
						       newRV(MUTABLE_SV(cv))),
                                               attrs)));
}

STATIC OP *
S_my_kid(pTHX_ OP *o, OP *attrs, OP **imopsp)
{
    dVAR;
    I32 type;

    PERL_ARGS_ASSERT_MY_KID;

    if (!o || (PL_parser && PL_parser->error_count))
	return o;

    type = o->op_type;
    if (PL_madskills && type == OP_NULL && o->op_flags & OPf_KIDS) {
	(void)my_kid(cUNOPo->op_first, attrs, imopsp);
	return o;
    }

    if (type == OP_LIST) {
        OP *kid;
	for (kid = cLISTOPo->op_first; kid; kid = kid->op_sibling)
	    my_kid(kid, attrs, imopsp);
    } else if (type == OP_UNDEF
#ifdef PERL_MAD
	       || type == OP_STUB
#endif
	       ) {
	return o;
    } else if (type == OP_RV2SV ||	/* "our" declaration */
	       type == OP_RV2AV ||
	       type == OP_RV2HV) { /* XXX does this let anything illegal in? */
	if (cUNOPo->op_first->op_type != OP_GV) { /* MJD 20011224 */
	    yyerror(Perl_form(aTHX_ "Can't declare %s in \"%s\"",
			OP_DESC(o),
			PL_parser->in_my == KEY_our
			    ? "our"
			    : PL_parser->in_my == KEY_state ? "state" : "my"));
	} else if (attrs) {
	    GV * const gv = cGVOPx_gv(cUNOPo->op_first);
	    PL_parser->in_my = FALSE;
	    PL_parser->in_my_stash = NULL;
	    apply_attrs(GvSTASH(gv),
			(type == OP_RV2SV ? GvSV(gv) :
			 type == OP_RV2AV ? MUTABLE_SV(GvAV(gv)) :
			 type == OP_RV2HV ? MUTABLE_SV(GvHV(gv)) : MUTABLE_SV(gv)),
			attrs, FALSE);
	}
	o->op_private |= OPpOUR_INTRO;
	return o;
    }
    else if (type != OP_PADSV &&
	     type != OP_PADAV &&
	     type != OP_PADHV &&
	     type != OP_PUSHMARK)
    {
	yyerror(Perl_form(aTHX_ "Can't declare %s in \"%s\"",
			  OP_DESC(o),
			  PL_parser->in_my == KEY_our
			    ? "our"
			    : PL_parser->in_my == KEY_state ? "state" : "my"));
	return o;
    }
    else if (attrs && type != OP_PUSHMARK) {
	HV *stash;

	PL_parser->in_my = FALSE;
	PL_parser->in_my_stash = NULL;

	/* check for C<my Dog $spot> when deciding package */
	stash = PAD_COMPNAME_TYPE(o->op_targ);
	if (!stash)
	    stash = PL_curstash;
	apply_attrs_my(stash, o, attrs, imopsp);
    }
    o->op_flags |= OPf_MOD;
    o->op_private |= OPpLVAL_INTRO;
    if (PL_parser->in_my == KEY_state)
	o->op_private |= OPpPAD_STATE;
    return o;
}

OP *
Perl_my_attrs(pTHX_ OP *o, OP *attrs)
{
    dVAR;
    OP *rops;
    int maybe_scalar = 0;

    PERL_ARGS_ASSERT_MY_ATTRS;

/* [perl #17376]: this appears to be premature, and results in code such as
   C< our(%x); > executing in list mode rather than void mode */
#if 0
    if (o->op_flags & OPf_PARENS)
	list(o);
    else
	maybe_scalar = 1;
#else
    maybe_scalar = 1;
#endif
    if (attrs)
	SAVEFREEOP(attrs);
    rops = NULL;
    o = my_kid(o, attrs, &rops);
    if (rops) {
	if (maybe_scalar && o->op_type == OP_PADSV) {
	    o = scalar(append_list(OP_LIST, (LISTOP*)rops, (LISTOP*)o));
	    o->op_private |= OPpLVAL_INTRO;
	}
	else
	    o = append_list(OP_LIST, (LISTOP*)o, (LISTOP*)rops);
    }
    PL_parser->in_my = FALSE;
    PL_parser->in_my_stash = NULL;
    return o;
}

OP *
Perl_sawparens(pTHX_ OP *o)
{
    PERL_UNUSED_CONTEXT;
    if (o)
	o->op_flags |= OPf_PARENS;
    return o;
}

OP *
Perl_bind_match(pTHX_ I32 type, OP *left, OP *right)
{
    OP *o;
    bool ismatchop = 0;
    const OPCODE ltype = left->op_type;
    const OPCODE rtype = right->op_type;

    PERL_ARGS_ASSERT_BIND_MATCH;

    if ( (ltype == OP_RV2AV || ltype == OP_RV2HV || ltype == OP_PADAV
	  || ltype == OP_PADHV) && ckWARN(WARN_MISC))
    {
      const char * const desc
	  = PL_op_desc[(rtype == OP_SUBST || rtype == OP_TRANS)
		       ? (int)rtype : OP_MATCH];
      const char * const sample = ((ltype == OP_RV2AV || ltype == OP_PADAV)
	     ? "@array" : "%hash");
      Perl_warner(aTHX_ packWARN(WARN_MISC),
             "Applying %s to %s will act on scalar(%s)",
             desc, sample, sample);
    }

    if (rtype == OP_CONST &&
	cSVOPx(right)->op_private & OPpCONST_BARE &&
	cSVOPx(right)->op_private & OPpCONST_STRICT)
    {
	no_bareword_allowed(right);
    }

    /* !~ doesn't make sense with s///r, so error on it for now */
    if (rtype == OP_SUBST && (cPMOPx(right)->op_pmflags & PMf_NONDESTRUCT) &&
	type == OP_NOT)
	yyerror("Using !~ with s///r doesn't make sense");

    ismatchop = rtype == OP_MATCH ||
		rtype == OP_SUBST ||
		rtype == OP_TRANS;
    if (ismatchop && right->op_private & OPpTARGET_MY) {
	right->op_targ = 0;
	right->op_private &= ~OPpTARGET_MY;
    }
    if (!(right->op_flags & OPf_STACKED) && ismatchop) {
	OP *newleft;

	right->op_flags |= OPf_STACKED;
	if (rtype != OP_MATCH &&
            ! (rtype == OP_TRANS &&
               right->op_private & OPpTRANS_IDENTICAL) &&
	    ! (rtype == OP_SUBST &&
	       (cPMOPx(right)->op_pmflags & PMf_NONDESTRUCT)))
	    newleft = mod(left, rtype);
	else
	    newleft = left;
	if (right->op_type == OP_TRANS)
	    o = newBINOP(OP_NULL, OPf_STACKED, scalar(newleft), right);
	else
	    o = prepend_elem(rtype, scalar(newleft), right);
	if (type == OP_NOT)
	    return newUNOP(OP_NOT, 0, scalar(o));
	return o;
    }
    else
	return bind_match(type, left,
		pmruntime(newPMOP(OP_MATCH, 0), right, 0));
}

OP *
Perl_invert(pTHX_ OP *o)
{
    if (!o)
	return NULL;
    return newUNOP(OP_NOT, OPf_SPECIAL, scalar(o));
}

OP *
Perl_scope(pTHX_ OP *o)
{
    dVAR;
    if (o) {
	if (o->op_flags & OPf_PARENS || PERLDB_NOOPT || PL_tainting) {
	    o = prepend_elem(OP_LINESEQ, newOP(OP_ENTER, 0), o);
	    o->op_type = OP_LEAVE;
	}
	else if (o->op_type == OP_LINESEQ) {
	    OP *kid;
	    o->op_type = OP_SCOPE;
	    kid = ((LISTOP*)o)->op_first;
	    if (kid->op_type == OP_NEXTSTATE || kid->op_type == OP_DBSTATE) {
		op_null(kid);

		/* The following deals with things like 'do {1 for 1}' */
		kid = kid->op_sibling;
		if (kid &&
		    (kid->op_type == OP_NEXTSTATE || kid->op_type == OP_DBSTATE))
		    op_null(kid);
	    }
	}
	else
	    o = newLISTOP(OP_SCOPE, 0, o, NULL);
    }
    return o;
}

int
Perl_block_start(pTHX_ int full)
{
    dVAR;
    const int retval = PL_savestack_ix;

    pad_block_start(full);
    SAVEHINTS();
    PL_hints &= ~HINT_BLOCK_SCOPE;
    SAVECOMPILEWARNINGS();
    PL_compiling.cop_warnings = DUP_WARNINGS(PL_compiling.cop_warnings);

    CALL_BLOCK_HOOKS(start, full);

    return retval;
}

OP*
Perl_block_end(pTHX_ I32 floor, OP *seq)
{
    dVAR;
    const int needblockscope = PL_hints & HINT_BLOCK_SCOPE;
    OP* retval = scalarseq(seq);

    CALL_BLOCK_HOOKS(pre_end, &retval);

    LEAVE_SCOPE(floor);
    CopHINTS_set(&PL_compiling, PL_hints);
    if (needblockscope)
	PL_hints |= HINT_BLOCK_SCOPE; /* propagate out */
    pad_leavemy();

    CALL_BLOCK_HOOKS(post_end, &retval);

    return retval;
}

/*
=head1 Compile-time scope hooks

=for apidoc Ao||blockhook_register

Register a set of hooks to be called when the Perl lexical scope changes
at compile time. See L<perlguts/"Compile-time scope hooks">.

=cut
*/

void
Perl_blockhook_register(pTHX_ BHK *hk)
{
    PERL_ARGS_ASSERT_BLOCKHOOK_REGISTER;

    Perl_av_create_and_push(aTHX_ &PL_blockhooks, newSViv(PTR2IV(hk)));
}

STATIC OP *
S_newDEFSVOP(pTHX)
{
    dVAR;
    const PADOFFSET offset = Perl_pad_findmy(aTHX_ STR_WITH_LEN("$_"), 0);
    if (offset == NOT_IN_PAD || PAD_COMPNAME_FLAGS_isOUR(offset)) {
	return newSVREF(newGVOP(OP_GV, 0, PL_defgv));
    }
    else {
	OP * const o = newOP(OP_PADSV, 0);
	o->op_targ = offset;
	return o;
    }
}

void
Perl_newPROG(pTHX_ OP *o)
{
    dVAR;

    PERL_ARGS_ASSERT_NEWPROG;

    if (PL_in_eval) {
	if (PL_eval_root)
		return;
	PL_eval_root = newUNOP(OP_LEAVEEVAL,
			       ((PL_in_eval & EVAL_KEEPERR)
				? OPf_SPECIAL : 0), o);
	PL_eval_root->op_private |= OPpREFCOUNTED;
	OpREFCNT_set(PL_eval_root, 1);
    }
    else {
	if (o->op_type == OP_STUB) {
	    PL_comppad_name = 0;
	    PL_compcv = 0;
	    S_op_destroy(aTHX_ o);
	    return;
	}
	PL_main_root = scope(sawparens(scalarvoid(o)));
	S_unknown(PL_main_root);
	PL_main_root->op_private |= OPpREFCOUNTED;
	OpREFCNT_set(PL_main_root, 1);
	finish_optree(PL_main_root);
	PL_curcop = &PL_compiling;
	PL_compcv = 0;

	/* Register with debugger */
	if (PERLDB_INTER) {
	    CV * const cv = get_cvs("DB::postponed", 0);
	    if (cv) {
		dSP;
		PUSHMARK(SP);
		XPUSHs(MUTABLE_SV(CopFILEGV(&PL_compiling)));
		PUTBACK;
		call_sv(MUTABLE_SV(cv), G_DISCARD);
	    }
	}
    }
}

OP *
Perl_localize(pTHX_ OP *o, I32 lex)
{
    dVAR;

    PERL_ARGS_ASSERT_LOCALIZE;

    if (o->op_flags & OPf_PARENS)
/* [perl #17376]: this appears to be premature, and results in code such as
   C< our(%x); > executing in list mode rather than void mode */
#if 0
	list(o);
#else
	NOOP;
#endif
    else {
	if ( PL_parser->bufptr > PL_parser->oldbufptr
	    && PL_parser->bufptr[-1] == ','
	    && ckWARN(WARN_PARENTHESIS))
	{
	    char *s = PL_parser->bufptr;
	    bool sigil = FALSE;

	    /* some heuristics to detect a potential error */
	    while (*s && (strchr(", \t\n", *s)))
		s++;

	    while (1) {
		if (*s && strchr("@$%*", *s) && *++s
		       && (isALNUM(*s) || UTF8_IS_CONTINUED(*s))) {
		    s++;
		    sigil = TRUE;
		    while (*s && (isALNUM(*s) || UTF8_IS_CONTINUED(*s)))
			s++;
		    while (*s && (strchr(", \t\n", *s)))
			s++;
		}
		else
		    break;
	    }
	    if (sigil && (*s == ';' || *s == '=')) {
		Perl_warner(aTHX_ packWARN(WARN_PARENTHESIS),
				"Parentheses missing around \"%s\" list",
				lex
				    ? (PL_parser->in_my == KEY_our
					? "our"
					: PL_parser->in_my == KEY_state
					    ? "state"
					    : "my")
				    : "local");
	    }
	}
    }
    if (lex)
	o = my(o);
    else
	o = mod(o, OP_NULL);		/* a bit kludgey */
    PL_parser->in_my = FALSE;
    PL_parser->in_my_stash = NULL;
    return o;
}

OP *
Perl_jmaybe(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_JMAYBE;

    if (o->op_type == OP_LIST) {
	OP * const o2
	    = newSVREF(newGVOP(OP_GV, 0, gv_fetchpvs(";", GV_ADD|GV_NOTQUAL, SVt_PV)));
	o = convert(OP_JOIN, 0, prepend_elem(OP_LIST, o2, o));
    }
    return o;
}

static OP *
S_fold_constants(pTHX_ register OP *o)
{
    VOL I32 type = o->op_type;

    PERL_ARGS_ASSERT_FOLD_CONSTANTS;

    if (PL_opargs[type] & OA_RETSCALAR)
	scalar(o);
    if (PL_opargs[type] & OA_TARGET && !o->op_targ)
	o->op_targ = pad_alloc(type, SVs_PADTMP);

    /* integerize op, unless it happens to be C<-foo>.
     * XXX should pp_i_negate() do magic string negation instead? */
    if ((PL_opargs[type] & OA_OTHERINT) && (PL_hints & HINT_INTEGER)
	&& !(type == OP_NEGATE && cUNOPo->op_first->op_type == OP_CONST
	     && (cUNOPo->op_first->op_private & OPpCONST_BARE)))
    {
	type = ++(o->op_type);
    }

    return o;
}

OP *
Perl_convert(pTHX_ I32 type, I32 flags, OP *o)
{
    dVAR;
    if (!o || o->op_type != OP_LIST)
	o = newLISTOP(OP_LIST, 0, o, NULL);
    else {
	o->op_flags &= ~OPf_WANT;
	o->op_context_known = FALSE;
    }

    o->op_type = (OPCODE)type;
    o->op_flags |= flags;

    o = CHECKOP(type, o);
    if (o->op_type != (unsigned)type)
	return o;

    return fold_constants(o);
}

/* List constructors */

OP *
Perl_append_elem(pTHX_ I32 type, OP *first, OP *last)
{
    if (!first)
	return last;

    if (!last)
	return first;

    if (first->op_type != (unsigned)type
	|| (type == OP_LIST && (first->op_flags & OPf_PARENS)))
    {
	return newLISTOP(type, 0, first, last);
    }

    if (first->op_flags & OPf_KIDS)
	((LISTOP*)first)->op_last->op_sibling = last;
    else {
	assert((PL_opargs[first->op_type] & OA_CLASS_MASK) >= OA_UNOP);
	first->op_flags |= OPf_KIDS;
	((LISTOP*)first)->op_first = last;
    }
    ((LISTOP*)first)->op_last = last;
    return first;
}

OP *
Perl_append_list(pTHX_ I32 type, LISTOP *first, LISTOP *last)
{
    if (!first)
	return (OP*)last;

    if (!last)
	return (OP*)first;

    if (first->op_type != (unsigned)type)
	return prepend_elem(type, (OP*)first, (OP*)last);

    if (last->op_type != (unsigned)type)
	return append_elem(type, (OP*)first, (OP*)last);

    first->op_last->op_sibling = last->op_first;
    first->op_last = last->op_last;
    first->op_flags |= (last->op_flags & OPf_KIDS);

#ifdef PERL_MAD
    if (last->op_first && first->op_madprop) {
	MADPROP *mp = last->op_first->op_madprop;
	if (mp) {
	    while (mp->mad_next)
		mp = mp->mad_next;
	    mp->mad_next = first->op_madprop;
	}
	else {
	    last->op_first->op_madprop = first->op_madprop;
	}
    }
    first->op_madprop = last->op_madprop;
    last->op_madprop = 0;
#endif

    S_op_destroy(aTHX_ (OP*)last);

    return (OP*)first;
}

OP *
Perl_prepend_elem(pTHX_ I32 type, OP *first, OP *last)
{
    if (!first)
	return last;

    if (!last)
	return first;

    if (last->op_type == (unsigned)type) {
	if (!(last->op_flags & OPf_KIDS)) {
	    ((LISTOP*)last)->op_last = first;
	    last->op_flags |= OPf_KIDS;
	}
	first->op_sibling = ((LISTOP*)last)->op_first;
	((LISTOP*)last)->op_first = first;
	if (type == OP_LIST) {
            if (!(first->op_flags & OPf_PARENS))
                last->op_flags &= ~OPf_PARENS;
	}
	assert((PL_opargs[last->op_type] & OA_CLASS_MASK) >= OA_UNOP);
	last->op_flags |= OPf_KIDS;
	return last;
    }

    return newLISTOP(type, 0, first, last);
}

/* Constructors */

#ifdef PERL_MAD
 
TOKEN *
Perl_newTOKEN(pTHX_ I32 optype, YYSTYPE lval, MADPROP* madprop)
{
    TOKEN *tk;
    Newxz(tk, 1, TOKEN);
    tk->tk_type = (OPCODE)optype;
    tk->tk_type = 12345;
    tk->tk_lval = lval;
    tk->tk_mad = madprop;
    return tk;
}

void
Perl_token_free(pTHX_ TOKEN* tk)
{
    PERL_ARGS_ASSERT_TOKEN_FREE;

    if (tk->tk_type != 12345)
	return;
    mad_free(tk->tk_mad);
    Safefree(tk);
}

void
Perl_token_getmad(pTHX_ TOKEN* tk, OP* o, char slot)
{
    MADPROP* mp;
    MADPROP* tm;

    PERL_ARGS_ASSERT_TOKEN_GETMAD;

    if (tk->tk_type != 12345) {
	Perl_warner(aTHX_ packWARN(WARN_MISC),
	     "Invalid TOKEN object ignored");
	return;
    }
    tm = tk->tk_mad;
    if (!tm)
	return;

    /* faked up qw list? */
    if (slot == '(' &&
	tm->mad_type == MAD_SV &&
	SvPVX((SV *)tm->mad_val)[0] == 'q')
	    slot = 'x';

    if (o) {
	mp = o->op_madprop;
	if (mp) {
	    for (;;) {
		/* pretend constant fold didn't happen? */
		if (mp->mad_key == 'f' &&
		    (o->op_type == OP_CONST ||
		     o->op_type == OP_GV) )
		{
		    token_getmad(tk,(OP*)mp->mad_val,slot);
		    return;
		}
		if (!mp->mad_next)
		    break;
		mp = mp->mad_next;
	    }
	    mp->mad_next = tm;
	    mp = mp->mad_next;
	}
	else {
	    o->op_madprop = tm;
	    mp = o->op_madprop;
	}
	if (mp->mad_key == 'X')
	    mp->mad_key = slot;	/* just change the first one */

	tk->tk_mad = 0;
    }
    else
	mad_free(tm);
    Safefree(tk);
}

void
Perl_op_getmad_weak(pTHX_ OP* from, OP* o, char slot)
{
    MADPROP* mp;
    if (!from)
	return;
    if (o) {
	mp = o->op_madprop;
	if (mp) {
	    for (;;) {
		/* pretend constant fold didn't happen? */
		if (mp->mad_key == 'f' &&
		    (o->op_type == OP_CONST ||
		     o->op_type == OP_GV) )
		{
		    op_getmad(from,(OP*)mp->mad_val,slot);
		    return;
		}
		if (!mp->mad_next)
		    break;
		mp = mp->mad_next;
	    }
	    mp->mad_next = newMADPROP(slot,MAD_OP,from,0);
	}
	else {
	    o->op_madprop = newMADPROP(slot,MAD_OP,from,0);
	}
    }
}

void
Perl_op_getmad(pTHX_ OP* from, OP* o, char slot)
{
    MADPROP* mp;
    if (!from)
	return;
    if (o) {
	mp = o->op_madprop;
	if (mp) {
	    for (;;) {
		/* pretend constant fold didn't happen? */
		if (mp->mad_key == 'f' &&
		    (o->op_type == OP_CONST ||
		     o->op_type == OP_GV) )
		{
		    op_getmad(from,(OP*)mp->mad_val,slot);
		    return;
		}
		if (!mp->mad_next)
		    break;
		mp = mp->mad_next;
	    }
	    mp->mad_next = newMADPROP(slot,MAD_OP,from,1);
	}
	else {
	    o->op_madprop = newMADPROP(slot,MAD_OP,from,1);
	}
    }
    else {
	PerlIO_printf(PerlIO_stderr(),
		      "DESTROYING op = %0"UVxf"\n", PTR2UV(from));
	op_free(from);
    }
}

void
Perl_prepend_madprops(pTHX_ MADPROP* mp, OP* o, char slot)
{
    MADPROP* tm;
    if (!mp || !o)
	return;
    if (slot)
	mp->mad_key = slot;
    tm = o->op_madprop;
    o->op_madprop = mp;
    for (;;) {
	if (!mp->mad_next)
	    break;
	mp = mp->mad_next;
    }
    mp->mad_next = tm;
}

void
Perl_append_madprops(pTHX_ MADPROP* tm, OP* o, char slot)
{
    if (!o)
	return;
    addmad(tm, &(o->op_madprop), slot);
}

void
Perl_addmad(pTHX_ MADPROP* tm, MADPROP** root, char slot)
{
    MADPROP* mp;
    if (!tm || !root)
	return;
    if (slot)
	tm->mad_key = slot;
    mp = *root;
    if (!mp) {
	*root = tm;
	return;
    }
    for (;;) {
	if (!mp->mad_next)
	    break;
	mp = mp->mad_next;
    }
    mp->mad_next = tm;
}

MADPROP *
Perl_newMADsv(pTHX_ char key, SV* sv)
{
    PERL_ARGS_ASSERT_NEWMADSV;

    return newMADPROP(key, MAD_SV, sv, 0);
}

MADPROP *
Perl_newMADPROP(pTHX_ char key, char type, void* val, I32 vlen)
{
    MADPROP *mp;
    Newxz(mp, 1, MADPROP);
    mp->mad_next = 0;
    mp->mad_key = key;
    mp->mad_vlen = vlen;
    mp->mad_type = type;
    mp->mad_val = val;
/*    PerlIO_printf(PerlIO_stderr(), "NEW  mp = %0x\n", mp);  */
    return mp;
}

void
Perl_mad_free(pTHX_ MADPROP* mp)
{
/*    PerlIO_printf(PerlIO_stderr(), "FREE mp = %0x\n", mp); */
    if (!mp)
	return;
    if (mp->mad_next)
	mad_free(mp->mad_next);
/*    if (PL_parser && PL_parser->lex_state != LEX_NOTPARSING && mp->mad_vlen)
	PerlIO_printf(PerlIO_stderr(), "DESTROYING '%c'=<%s>\n", mp->mad_key & 255, mp->mad_val); */
    switch (mp->mad_type) {
    case MAD_NULL:
	break;
    case MAD_PV:
	Safefree((char*)mp->mad_val);
	break;
    case MAD_OP:
	if (mp->mad_vlen)	/* vlen holds "strong/weak" boolean */
	    op_free((OP*)mp->mad_val);
	break;
    case MAD_SV:
	sv_free(MUTABLE_SV(mp->mad_val));
	break;
    default:
	PerlIO_printf(PerlIO_stderr(), "Unrecognized mad\n");
	break;
    }
    Safefree(mp);
}

#endif

/*
=head1 Optree construction

=for apidoc Am|OP *|newNULLLIST

Constructs, checks, and returns a new C<stub> op, which represents an
empty list expression.

=cut
*/

OP *
Perl_newNULLLIST(pTHX)
{
    return newOP(OP_STUB, 0);
}

static OP *
S_force_list(pTHX_ OP *o)
{
    if (!o || o->op_type != OP_LIST)
	o = newLISTOP(OP_LIST, 0, o, NULL);
    return o;
}

/*
=for apidoc Am|OP *|newLISTOP|I32 type|I32 flags|OP *first|OP *last

Constructs, checks, and returns an op of any list type.  I<type> is
the opcode.  I<flags> gives the eight bits of C<op_flags>, except that
C<OPf_KIDS> will be set automatically if required.  I<first> and I<last>
supply up to two ops to be direct children of the list op; they are
consumed by this function and become part of the constructed op tree.

=cut
*/

OP *
Perl_newLISTOP(pTHX_ I32 type, I32 flags, OP *first, OP *last)
{
    dVAR;
    LISTOP *listop;

    assert((PL_opargs[type] & OA_CLASS_MASK) == OA_LISTOP);

    NewOp(1101, listop, 1, LISTOP);

    listop->op_type = (OPCODE)type;
    if (first || last)
	flags |= OPf_KIDS;
    listop->op_flags = (U8)flags;

    if (!last && first)
	last = first;
    else if (!first && last)
	first = last;
    else if (first)
	first->op_sibling = last;
    listop->op_first = first;
    listop->op_last = last;

    return CHECKOP(type, listop);
}

/*
=for apidoc Am|OP *|newOP|I32 type|I32 flags

Constructs, checks, and returns an op of any base type (any type that
has no extra fields).  I<type> is the opcode.  I<flags> gives the
eight bits of C<op_flags>, and, shifted up eight bits, the eight bits
of C<op_private>.

=cut
*/

OP *
Perl_newOP(pTHX_ I32 type, I32 flags)
{
    dVAR;
    OP *o;

    assert((PL_opargs[type] & OA_CLASS_MASK) == OA_BASEOP
	|| (PL_opargs[type] & OA_CLASS_MASK) == OA_BASEOP_OR_UNOP
	|| (PL_opargs[type] & OA_CLASS_MASK) == OA_FILESTATOP
	|| (PL_opargs[type] & OA_CLASS_MASK) == OA_LOOPEXOP);

    NewOp(1101, o, 1, OP);
    o->op_type = (OPCODE)type;
    o->op_flags = (U8)flags;
    o->op_latefree = 0;
    o->op_latefreed = 0;
    o->op_attached = 0;

    o->op_private = (U8)(0 | (flags >> 8));
    if (PL_opargs[type] & OA_RETSCALAR)
	scalar(o);
    if (PL_opargs[type] & OA_TARGET)
	o->op_targ = pad_alloc(type, SVs_PADTMP);
    return CHECKOP(type, o);
}

/*
=for apidoc Am|OP *|newUNOP|I32 type|I32 flags|OP *first

Constructs, checks, and returns an op of any unary type.  I<type> is
the opcode.  I<flags> gives the eight bits of C<op_flags>, except that
C<OPf_KIDS> will be set automatically if required, and, shifted up eight
bits, the eight bits of C<op_private>, except that the bit with value 1
is automatically set.  I<first> supplies an optional op to be the direct
child of the unary op; it is consumed by this function and become part
of the constructed op tree.

=cut
*/

OP *
Perl_newUNOP(pTHX_ I32 type, I32 flags, OP *first)
{
    dVAR;
    UNOP *unop;

    assert((PL_opargs[type] & OA_CLASS_MASK) == OA_UNOP
	|| (PL_opargs[type] & OA_CLASS_MASK) == OA_BASEOP_OR_UNOP
	|| (PL_opargs[type] & OA_CLASS_MASK) == OA_FILESTATOP
	|| (PL_opargs[type] & OA_CLASS_MASK) == OA_LOOPEXOP
	|| type == OP_SASSIGN
	|| type == OP_ENTERTRY
	|| type == OP_NULL );

    if (!first)
	first = newOP(OP_STUB, 0);
    if (PL_opargs[type] & OA_MARK)
    	first = force_list(first);

    NewOp(1101, unop, 1, UNOP);
    unop->op_type = (OPCODE)type;
    unop->op_first = first;
    unop->op_flags = (U8)(flags | OPf_KIDS);
    unop->op_private = (U8)(1 | (flags >> 8));
    unop = (UNOP*) CHECKOP(type, unop);

    return fold_constants((OP *) unop);
}

/*
=for apidoc Am|OP *|newBINOP|I32 type|I32 flags|OP *first|OP *last

Constructs, checks, and returns an op of any binary type.  I<type>
is the opcode.  I<flags> gives the eight bits of C<op_flags>, except
that C<OPf_KIDS> will be set automatically, and, shifted up eight bits,
the eight bits of C<op_private>, except that the bit with value 1 or
2 is automatically set as required.  I<first> and I<last> supply up to
two ops to be the direct children of the binary op; they are consumed
by this function and become part of the constructed op tree.

=cut
*/

OP *
Perl_newBINOP(pTHX_ I32 type, I32 flags, OP *first, OP *last)
{
    dVAR;
    BINOP *binop;

    assert((PL_opargs[type] & OA_CLASS_MASK) == OA_BINOP
	|| type == OP_SASSIGN || type == OP_NULL );

    NewOp(1101, binop, 1, BINOP);

    if (!first)
	first = newOP(OP_NULL, 0);

    binop->op_type = (OPCODE)type;
    binop->op_first = first;
    binop->op_flags = (U8)(flags | OPf_KIDS);
    if (!last) {
	last = first;
	binop->op_private = (U8)(1 | (flags >> 8));
    }
    else {
	binop->op_private = (U8)(2 | (flags >> 8));
	first->op_sibling = last;
    }

    binop = (BINOP*)CHECKOP(type, binop);
    if (binop->op_type != (OPCODE)type)
	return (OP*)binop;

    binop->op_last = binop->op_first->op_sibling;

    return fold_constants((OP *)binop);
}

static int uvcompare(const void *a, const void *b)
    __attribute__nonnull__(1)
    __attribute__nonnull__(2)
    __attribute__pure__;
static int uvcompare(const void *a, const void *b)
{
    if (*((const UV *)a) < (*(const UV *)b))
	return -1;
    if (*((const UV *)a) > (*(const UV *)b))
	return 1;
    if (*((const UV *)a+1) < (*(const UV *)b+1))
	return -1;
    if (*((const UV *)a+1) > (*(const UV *)b+1))
	return 1;
    return 0;
}

static OP *
S_pmtrans(pTHX_ OP *o, OP *expr, OP *repl)
{
    dVAR;
    SV * const tstr = ((SVOP*)expr)->op_sv;
    SV * const rstr =
#ifdef PERL_MAD
			(repl->op_type == OP_NULL)
			    ? ((SVOP*)((LISTOP*)repl)->op_first)->op_sv :
#endif
			      ((SVOP*)repl)->op_sv;
    STRLEN tlen;
    STRLEN rlen;
    const U8 *t = (U8*)SvPV_const(tstr, tlen);
    const U8 *r = (U8*)SvPV_const(rstr, rlen);
    register I32 i;
    register I32 j;
    I32 grows = 0;
    register short *tbl;

    const I32 complement = o->op_private & OPpTRANS_COMPLEMENT;
    const I32 squash     = o->op_private & OPpTRANS_SQUASH;
    I32 del              = o->op_private & OPpTRANS_DELETE;
    SV* swash;

    PERL_ARGS_ASSERT_PMTRANS;

    PL_hints |= HINT_BLOCK_SCOPE;

    if (SvUTF8(tstr))
        o->op_private |= OPpTRANS_FROM_UTF;

    if (SvUTF8(rstr))
        o->op_private |= OPpTRANS_TO_UTF;

    if (o->op_private & (OPpTRANS_FROM_UTF|OPpTRANS_TO_UTF)) {
	SV* const listsv = newSVpvs("# comment\n");
	SV* transv = NULL;
	const U8* tend = t + tlen;
	const U8* rend = r + rlen;
	STRLEN ulen;
	UV tfirst = 1;
	UV tlast = 0;
	IV tdiff;
	UV rfirst = 1;
	UV rlast = 0;
	IV rdiff;
	IV diff;
	I32 none = 0;
	U32 max = 0;
	I32 bits;
	I32 havefinal = 0;
	U32 final = 0;
	const I32 from_utf  = o->op_private & OPpTRANS_FROM_UTF;
	const I32 to_utf    = o->op_private & OPpTRANS_TO_UTF;
	U8* tsave = NULL;
	U8* rsave = NULL;
	const U32 flags = UTF8_ALLOW_DEFAULT;

	if (!from_utf) {
	    STRLEN len = tlen;
	    t = tsave = bytes_to_utf8(t, &len);
	    tend = t + len;
	}
	if (!to_utf && rlen) {
	    STRLEN len = rlen;
	    r = rsave = bytes_to_utf8(r, &len);
	    rend = r + len;
	}

/* There are several snags with this code on EBCDIC:
   1. 0xFF is a legal UTF-EBCDIC byte (there are no illegal bytes).
   2. scan_const() in toke.c has encoded chars in native encoding which makes
      ranges at least in EBCDIC 0..255 range the bottom odd.
*/

	if (complement) {
	    U8 tmpbuf[UTF8_MAXBYTES+1];
	    UV *cp;
	    UV nextmin = 0;
	    Newx(cp, 2*tlen, UV);
	    i = 0;
	    transv = newSVpvs("");
	    while (t < tend) {
		cp[2*i] = utf8n_to_uvuni(t, tend-t, &ulen, flags);
		t += ulen;
		if (t < tend && NATIVE_TO_UTF(*t) == 0xff) {
		    t++;
		    cp[2*i+1] = utf8n_to_uvuni(t, tend-t, &ulen, flags);
		    t += ulen;
		}
		else {
		 cp[2*i+1] = cp[2*i];
		}
		i++;
	    }
	    qsort(cp, i, 2*sizeof(UV), uvcompare);
	    for (j = 0; j < i; j++) {
		UV  val = cp[2*j];
		diff = val - nextmin;
		if (diff > 0) {
		    t = uvuni_to_utf8(tmpbuf,nextmin);
		    sv_catpvn(transv, (char*)tmpbuf, t - tmpbuf);
		    if (diff > 1) {
			U8  range_mark = UTF_TO_NATIVE(0xff);
			t = uvuni_to_utf8(tmpbuf, val - 1);
			sv_catpvn(transv, (char *)&range_mark, 1);
			sv_catpvn(transv, (char*)tmpbuf, t - tmpbuf);
		    }
	        }
		val = cp[2*j+1];
		if (val >= nextmin)
		    nextmin = val + 1;
	    }
	    t = uvuni_to_utf8(tmpbuf,nextmin);
	    sv_catpvn(transv, (char*)tmpbuf, t - tmpbuf);
	    {
		U8 range_mark = UTF_TO_NATIVE(0xff);
		sv_catpvn(transv, (char *)&range_mark, 1);
	    }
	    t = uvuni_to_utf8_flags(tmpbuf, 0x7fffffff,
				    UNICODE_ALLOW_SUPER);
	    sv_catpvn(transv, (char*)tmpbuf, t - tmpbuf);
	    t = (const U8*)SvPVX_const(transv);
	    tlen = SvCUR(transv);
	    tend = t + tlen;
	    Safefree(cp);
	}
	else if (!rlen && !del) {
	    r = t; rlen = tlen; rend = tend;
	}
	if (!squash) {
		if ((!rlen && !del) || t == r ||
		    (tlen == rlen && memEQ((char *)t, (char *)r, tlen)))
		{
		    o->op_private |= OPpTRANS_IDENTICAL;
		}
	}

	while (t < tend || tfirst <= tlast) {
	    /* see if we need more "t" chars */
	    if (tfirst > tlast) {
		tfirst = (I32)utf8n_to_uvuni(t, tend - t, &ulen, flags);
		t += ulen;
		if (t < tend && NATIVE_TO_UTF(*t) == 0xff) {	/* illegal utf8 val indicates range */
		    t++;
		    tlast = (I32)utf8n_to_uvuni(t, tend - t, &ulen, flags);
		    t += ulen;
		}
		else
		    tlast = tfirst;
	    }

	    /* now see if we need more "r" chars */
	    if (rfirst > rlast) {
		if (r < rend) {
		    rfirst = (I32)utf8n_to_uvuni(r, rend - r, &ulen, flags);
		    r += ulen;
		    if (r < rend && NATIVE_TO_UTF(*r) == 0xff) {	/* illegal utf8 val indicates range */
			r++;
			rlast = (I32)utf8n_to_uvuni(r, rend - r, &ulen, flags);
			r += ulen;
		    }
		    else
			rlast = rfirst;
		}
		else {
		    if (!havefinal++)
			final = rlast;
		    rfirst = rlast = 0xffffffff;
		}
	    }

	    /* now see which range will peter our first, if either. */
	    tdiff = tlast - tfirst;
	    rdiff = rlast - rfirst;

	    if (tdiff <= rdiff)
		diff = tdiff;
	    else
		diff = rdiff;

	    if (rfirst == 0xffffffff) {
		diff = tdiff;	/* oops, pretend rdiff is infinite */
		if (diff > 0)
		    Perl_sv_catpvf(aTHX_ listsv, "%04lx\t%04lx\tXXXX\n",
				   (long)tfirst, (long)tlast);
		else
		    Perl_sv_catpvf(aTHX_ listsv, "%04lx\t\tXXXX\n", (long)tfirst);
	    }
	    else {
		if (diff > 0)
		    Perl_sv_catpvf(aTHX_ listsv, "%04lx\t%04lx\t%04lx\n",
				   (long)tfirst, (long)(tfirst + diff),
				   (long)rfirst);
		else
		    Perl_sv_catpvf(aTHX_ listsv, "%04lx\t\t%04lx\n",
				   (long)tfirst, (long)rfirst);

		if (rfirst + diff > max)
		    max = rfirst + diff;
		if (!grows)
		    grows = (tfirst < rfirst &&
			     UNISKIP(tfirst) < UNISKIP(rfirst + diff));
		rfirst += diff + 1;
	    }
	    tfirst += diff + 1;
	}

	none = ++max;
	if (del)
	    del = ++max;

	if (max > 0xffff)
	    bits = 32;
	else if (max > 0xff)
	    bits = 16;
	else
	    bits = 8;

	PerlMemShared_free(cPVOPo->op_pv);
	cPVOPo->op_pv = NULL;

	swash = MUTABLE_SV(swash_init("utf8", "", listsv, bits, none));
#ifdef USE_ITHREADS
	cPADOPo->op_padix = pad_alloc(OP_TRANS, SVs_PADTMP);
	SvREFCNT_dec(PAD_SVl(cPADOPo->op_padix));
	PAD_SETSV(cPADOPo->op_padix, swash);
	SvPADTMP_on(swash);
	SvREADONLY_on(swash);
#else
	cSVOPo->op_sv = swash;
#endif
	SvREFCNT_dec(listsv);
	SvREFCNT_dec(transv);

	if (!del && havefinal && rlen)
	    (void)hv_store(MUTABLE_HV(SvRV(swash)), "FINAL", 5,
			   newSVuv((UV)final), 0);

	if (grows)
	    o->op_private |= OPpTRANS_GROWS;

	Safefree(tsave);
	Safefree(rsave);

#ifdef PERL_MAD
	op_getmad(expr,o,'e');
	op_getmad(repl,o,'r');
#else
	op_free(expr);
	op_free(repl);
#endif
	return o;
    }

    tbl = (short*)cPVOPo->op_pv;
    if (complement) {
	Zero(tbl, 256, short);
	for (i = 0; i < (I32)tlen; i++)
	    tbl[t[i]] = -1;
	for (i = 0, j = 0; i < 256; i++) {
	    if (!tbl[i]) {
		if (j >= (I32)rlen) {
		    if (del)
			tbl[i] = -2;
		    else if (rlen)
			tbl[i] = r[j-1];
		    else
			tbl[i] = (short)i;
		}
		else {
		    if (i < 128 && r[j] >= 128)
			grows = 1;
		    tbl[i] = r[j++];
		}
	    }
	}
	if (!del) {
	    if (!rlen) {
		j = rlen;
		if (!squash)
		    o->op_private |= OPpTRANS_IDENTICAL;
	    }
	    else if (j >= (I32)rlen)
		j = rlen - 1;
	    else {
		tbl = 
		    (short *)
		    PerlMemShared_realloc(tbl,
					  (0x101+rlen-j) * sizeof(short));
		cPVOPo->op_pv = (char*)tbl;
	    }
	    tbl[0x100] = (short)(rlen - j);
	    for (i=0; i < (I32)rlen - j; i++)
		tbl[0x101+i] = r[j+i];
	}
    }
    else {
	if (!rlen && !del) {
	    r = t; rlen = tlen;
	    if (!squash)
		o->op_private |= OPpTRANS_IDENTICAL;
	}
	else if (!squash && rlen == tlen && memEQ((char*)t, (char*)r, tlen)) {
	    o->op_private |= OPpTRANS_IDENTICAL;
	}
	for (i = 0; i < 256; i++)
	    tbl[i] = -1;
	for (i = 0, j = 0; i < (I32)tlen; i++,j++) {
	    if (j >= (I32)rlen) {
		if (del) {
		    if (tbl[t[i]] == -1)
			tbl[t[i]] = -2;
		    continue;
		}
		--j;
	    }
	    if (tbl[t[i]] == -1) {
		if (t[i] < 128 && r[j] >= 128)
		    grows = 1;
		tbl[t[i]] = r[j];
	    }
	}
    }

    if(del && rlen == tlen) {
	Perl_ck_warner(aTHX_ packWARN(WARN_MISC), "Useless use of /d modifier in transliteration operator"); 
    } else if(rlen > tlen) {
	Perl_ck_warner(aTHX_ packWARN(WARN_MISC), "Replacement list is longer than search list");
    }

    if (grows)
	o->op_private |= OPpTRANS_GROWS;
#ifdef PERL_MAD
    op_getmad(expr,o,'e');
    op_getmad(repl,o,'r');
#else
    op_free(expr);
    op_free(repl);
#endif

    return o;
}

/*
=for apidoc Am|OP *|newPMOP|I32 type|I32 flags

Constructs, checks, and returns an op of any pattern matching type.
I<type> is the opcode.  I<flags> gives the eight bits of C<op_flags>
and, shifted up eight bits, the eight bits of C<op_private>.

=cut
*/

OP *
Perl_newPMOP(pTHX_ I32 type, I32 flags)
{
    dVAR;
    PMOP *pmop;

    assert((PL_opargs[type] & OA_CLASS_MASK) == OA_PMOP);

    NewOp(1101, pmop, 1, PMOP);
    pmop->op_type = (OPCODE)type;
    pmop->op_flags = (U8)flags;
    pmop->op_private = (U8)(0 | (flags >> 8));

    if (PL_hints & HINT_RE_TAINT)
	pmop->op_pmflags |= PMf_RETAINT;
    if (PL_hints & HINT_LOCALE)
	pmop->op_pmflags |= PMf_LOCALE;


#ifdef USE_ITHREADS
    assert(SvPOK(PL_regex_pad[0]));
    if (SvCUR(PL_regex_pad[0])) {
	/* Pop off the "packed" IV from the end.  */
	SV *const repointer_list = PL_regex_pad[0];
	const char *p = SvEND(repointer_list) - sizeof(IV);
	const IV offset = *((IV*)p);

	assert(SvCUR(repointer_list) % sizeof(IV) == 0);

	SvEND_set(repointer_list, p);

	pmop->op_pmoffset = offset;
	/* This slot should be free, so assert this:  */
	assert(PL_regex_pad[offset] == &PL_sv_undef);
    } else {
	SV * const repointer = &PL_sv_undef;
	av_push(PL_regex_padav, repointer);
	pmop->op_pmoffset = av_len(PL_regex_padav);
	PL_regex_pad = AvARRAY(PL_regex_padav);
    }
#endif

    return CHECKOP(type, pmop);
}

static bool
S_repl_is_constant(OP* o, bool * const repl_has_varsp)
{
    PERL_ARGS_ASSERT_REPL_IS_CONSTANT;
    if (o->op_type == OP_SCOPE
	|| o->op_type == OP_LEAVE
	|| (PL_opargs[o->op_type] & OA_DANGEROUS)) {
	if (o->op_type == OP_GV) {
	    GV * const gv = cGVOPx_gv(o);
	    *repl_has_varsp = 1;
	    if (strchr("&`'123456789+-\016\022", *GvENAME(gv)))
		return FALSE;
	}
	else if (o->op_type == OP_RV2CV)
	    return FALSE;
	else if (o->op_type == OP_RV2SV ||
	    o->op_type == OP_RV2AV ||
	    o->op_type == OP_RV2HV ||
	    o->op_type == OP_RV2GV) {
	    if (cUNOPo->op_first->op_type != OP_GV)	/*funny deref?*/
		return FALSE;
	}
	else if (o->op_type == OP_PADSV ||
	    o->op_type == OP_PADAV ||
	    o->op_type == OP_PADHV ||
	    o->op_type == OP_PADANY)
	    {
		*repl_has_varsp = 1;
	    }
	else if (o->op_type == OP_PUSHRE)
	    NOOP; /* Okay here, dangerous in newASSIGNOP */
	else
	    return FALSE;
    }
    if (o->op_flags & OPf_KIDS) {
	OP* kid;
	for (kid = cUNOPo->op_first; kid; kid=kid->op_sibling) {
	    if (!repl_is_constant(kid, repl_has_varsp))
		return FALSE;
	}
    }
    return TRUE;
}

/* Given some sort of match op o, and an expression expr containing a
 * pattern, either compile expr into a regex and attach it to o (if it's
 * constant), or convert expr into a runtime regcomp op sequence (if it's
 * not)
 *
 * isreg indicates that the pattern is part of a regex construct, eg
 * $x =~ /pattern/ or split /pattern/, as opposed to $x =~ $pattern or
 * split "pattern", which aren't. In the former case, expr will be a list
 * if the pattern contains more than one term (eg /a$b/) or if it contains
 * a replacement, ie s/// or tr///.
 */

OP *
Perl_pmruntime(pTHX_ OP *o, OP *expr, bool isreg)
{
    dVAR;
    PMOP *pm;
    LOGOP *rcop;
    bool repl_has_vars = FALSE;
    OP* repl = NULL;
    bool reglist;

    PERL_ARGS_ASSERT_PMRUNTIME;

    if (o->op_type == OP_SUBST || o->op_type == OP_TRANS) {
	/* last element in list is the replacement; pop it */
	OP* kid;
	repl = cLISTOPx(expr)->op_last;
	kid = cLISTOPx(expr)->op_first;
	while (kid->op_sibling != repl)
	    kid = kid->op_sibling;
	kid->op_sibling = NULL;
	cLISTOPx(expr)->op_last = kid;
    }

    if (isreg && expr->op_type == OP_LIST &&
	cLISTOPx(expr)->op_first == cLISTOPx(expr)->op_last)
    {
	/* convert single element list to element */
	OP* const oe = expr;
	expr = cLISTOPx(oe)->op_first;
	cLISTOPx(oe)->op_first = NULL;
	cLISTOPx(oe)->op_last = NULL;
	op_free(oe);
    }

    if (o->op_type == OP_TRANS) {
	return pmtrans(o, expr, repl);
    }

    reglist = isreg && expr->op_type == OP_LIST;
    if (reglist)
	op_null(expr);

    PL_hints |= HINT_BLOCK_SCOPE;
    pm = (PMOP*)o;

    if (expr->op_type == OP_CONST) {
	SV *pat = ((SVOP*)expr)->op_sv;
	U32 pm_flags = pm->op_pmflags & PMf_COMPILETIME;

	if (o->op_flags & OPf_SPECIAL)
	    pm_flags |= RXf_SPLIT;

	if (DO_UTF8(pat)) {
	    assert (SvUTF8(pat));
	} else if (SvUTF8(pat)) {
	    /* Not doing UTF-8, despite what the SV says. Is this only if we're
	       trapped in use 'bytes'?  */
	    /* Make a copy of the octet sequence, but without the flag on, as
	       the compiler now honours the SvUTF8 flag on pat.  */
	    STRLEN len;
	    const char *const p = SvPV(pat, len);
	    pat = newSVpvn_flags(p, len, SVs_TEMP);
	}

	PM_SETRE(pm, CALLREGCOMP(pat, pm_flags));

#ifdef PERL_MAD
	op_getmad(expr,(OP*)pm,'e');
#else
	op_free(expr);
#endif
    }
    else {
	if (pm->op_pmflags & PMf_KEEP || !(PL_hints & HINT_RE_EVAL))
	    expr = newUNOP((!(PL_hints & HINT_RE_EVAL)
			    ? OP_REGCRESET
			    : OP_REGCMAYBE),0,expr);

	NewOp(1101, rcop, 1, LOGOP);
	rcop->op_type = OP_REGCOMP;
	rcop->op_first = scalar(expr);
	rcop->op_flags |= OPf_KIDS
			    | ((PL_hints & HINT_RE_EVAL) ? OPf_SPECIAL : 0)
			    | (reglist ? OPf_STACKED : 0);
	rcop->op_private = 1;
	rcop->op_other = o;
	if (reglist)
	    rcop->op_targ = pad_alloc(rcop->op_type, SVs_PADTMP);

	/* /$x/ may cause an eval, since $x might be qr/(?{..})/  */
	PL_cv_has_eval = 1;

	prepend_elem(o->op_type, scalar((OP*)rcop), o);
    }

    if (repl) {
	OP *curop;
	if (pm->op_pmflags & PMf_EVAL) {
	    curop = NULL;
	    if (CopLINE(PL_curcop) < (line_t)PL_parser->multi_end)
		CopLINE_set(PL_curcop, (line_t)PL_parser->multi_end);
	}
	else if (repl->op_type == OP_CONST)
	    curop = repl;
	else {
	    if (repl_is_constant(repl, &repl_has_vars))
		curop = repl;
	    else
		curop = NULL;
	}
	if (curop == repl
	    && !(repl_has_vars
		 && (!PM_GETRE(pm)
		     || RX_EXTFLAGS(PM_GETRE(pm)) & RXf_EVAL_SEEN)))
	{
	    pm->op_pmflags |= PMf_CONST;	/* const for long enough */
	    prepend_elem(o->op_type, scalar(repl), o);
	}
	else {
	    if (curop == repl && !PM_GETRE(pm)) { /* Has variables. */
		pm->op_pmflags |= PMf_MAYBE_CONST;
	    }
	    NewOp(1101, rcop, 1, LOGOP);
	    rcop->op_type = OP_SUBSTCONT;
	    rcop->op_first = scalar(repl);
	    rcop->op_flags |= OPf_KIDS;
	    rcop->op_private = 1;
	    rcop->op_other = o;

	    pm->op_pmreplrootu.op_pmreplroot = scalar((OP*)rcop);
	    assert(!(pm->op_pmflags & PMf_ONCE));
	}
    }

    return (OP*)pm;
}

/*
=for apidoc Am|OP *|newSVOP|I32 type|I32 flags|SV *sv

Constructs, checks, and returns an op of any type that involves an
embedded SV.  I<type> is the opcode.  I<flags> gives the eight bits
of C<op_flags>.  I<sv> gives the SV to embed in the op; this function
takes ownership of one reference to it.

=cut
*/

OP *
Perl_newSVOP(pTHX_ I32 type, I32 flags, SV *sv)
{
    dVAR;
    SVOP *svop;

    PERL_ARGS_ASSERT_NEWSVOP;

    assert((PL_opargs[type] & OA_CLASS_MASK) == OA_SVOP
	|| (PL_opargs[type] & OA_CLASS_MASK) == OA_PVOP_OR_SVOP
	|| (PL_opargs[type] & OA_CLASS_MASK) == OA_FILESTATOP);

    NewOp(1101, svop, 1, SVOP);
    svop->op_type = (OPCODE)type;
    svop->op_sv = sv;
    svop->op_flags = (U8)flags;
    if (PL_opargs[type] & OA_RETSCALAR)
	scalar((OP*)svop);
    if (PL_opargs[type] & OA_TARGET)
	svop->op_targ = pad_alloc(type, SVs_PADTMP);
    return CHECKOP(type, svop);
}

#ifdef USE_ITHREADS

/*
=for apidoc Am|OP *|newPADOP|I32 type|I32 flags|SV *sv

Constructs, checks, and returns an op of any type that involves a
reference to a pad element.  I<type> is the opcode.  I<flags> gives the
eight bits of C<op_flags>.  A pad slot is automatically allocated, and
is populated with I<sv>; this function takes ownership of one reference
to it.

This function only exists if Perl has been compiled to use ithreads.

=cut
*/

OP *
Perl_newPADOP(pTHX_ I32 type, I32 flags, SV *sv)
{
    dVAR;
    PADOP *padop;

    PERL_ARGS_ASSERT_NEWPADOP;

    assert((PL_opargs[type] & OA_CLASS_MASK) == OA_SVOP
	|| (PL_opargs[type] & OA_CLASS_MASK) == OA_PVOP_OR_SVOP
	|| (PL_opargs[type] & OA_CLASS_MASK) == OA_FILESTATOP);

    NewOp(1101, padop, 1, PADOP);
    padop->op_type = (OPCODE)type;
    padop->op_padix = pad_alloc(type, SVs_PADTMP);
    SvREFCNT_dec(PAD_SVl(padop->op_padix));
    PAD_SETSV(padop->op_padix, sv);
    assert(sv);
    SvPADTMP_on(sv);
    padop->op_flags = (U8)flags;
    if (PL_opargs[type] & OA_RETSCALAR)
	scalar((OP*)padop);
    if (PL_opargs[type] & OA_TARGET)
	padop->op_targ = pad_alloc(type, SVs_PADTMP);
    return CHECKOP(type, padop);
}

#endif /* !USE_ITHREADS */

/*
=for apidoc Am|OP *|newGVOP|I32 type|I32 flags|GV *gv

Constructs, checks, and returns an op of any type that involves an
embedded reference to a GV.  I<type> is the opcode.  I<flags> gives the
eight bits of C<op_flags>.  I<gv> identifies the GV that the op should
reference; calling this function does not transfer ownership of any
reference to it.

=cut
*/

OP *
Perl_newGVOP(pTHX_ I32 type, I32 flags, GV *gv)
{
    dVAR;

    PERL_ARGS_ASSERT_NEWGVOP;

#ifdef USE_ITHREADS
    GvIN_PAD_on(gv);
    return newPADOP(type, flags, SvREFCNT_inc_simple_NN(gv));
#else
    return newSVOP(type, flags, SvREFCNT_inc_simple_NN(gv));
#endif
}

/*
=for apidoc Am|OP *|newPVOP|I32 type|I32 flags|char *pv

Constructs, checks, and returns an op of any type that involves an
embedded C-level pointer (PV).  I<type> is the opcode.  I<flags> gives
the eight bits of C<op_flags>.  I<pv> supplies the C-level pointer, which
must have been allocated using L</PerlMemShared_malloc>; the memory will
be freed when the op is destroyed.

=cut
*/

OP *
Perl_newPVOP(pTHX_ I32 type, I32 flags, char *pv)
{
    dVAR;
    PVOP *pvop;

    assert((PL_opargs[type] & OA_CLASS_MASK) == OA_PVOP_OR_SVOP
	|| (PL_opargs[type] & OA_CLASS_MASK) == OA_LOOPEXOP);

    NewOp(1101, pvop, 1, PVOP);
    pvop->op_type = (OPCODE)type;
    pvop->op_pv = pv;
    pvop->op_flags = (U8)flags;
    if (PL_opargs[type] & OA_RETSCALAR)
	scalar((OP*)pvop);
    if (PL_opargs[type] & OA_TARGET)
	pvop->op_targ = pad_alloc(type, SVs_PADTMP);
    return CHECKOP(type, pvop);
}

#ifdef PERL_MAD
OP*
#else
void
#endif
Perl_package(pTHX_ OP *o)
{
    dVAR;
    SV *const sv = cSVOPo->op_sv;
#ifdef PERL_MAD
    OP *pegop;
#endif

    PERL_ARGS_ASSERT_PACKAGE;

    save_hptr(&PL_curstash);
    save_item(PL_curstname);

    PL_curstash = gv_stashsv(sv, GV_ADD);

    sv_setsv(PL_curstname, sv);

    PL_hints |= HINT_BLOCK_SCOPE;
    PL_parser->copline = NOLINE;
    PL_parser->expect = XSTATE;

#ifndef PERL_MAD
    op_free(o);
#else
    if (!PL_madskills) {
	op_free(o);
	return NULL;
    }

    pegop = newOP(OP_NULL,0);
    op_getmad(o,pegop,'P');
    return pegop;
#endif
}

void
Perl_package_version( pTHX_ OP *v )
{
    dVAR;
    U32 savehints = PL_hints;
    PERL_ARGS_ASSERT_PACKAGE_VERSION;
    PL_hints &= ~HINT_STRICT_VARS;
    sv_setsv( GvSV(gv_fetchpvs("VERSION", GV_ADDMULTI, SVt_PV)), cSVOPx(v)->op_sv );
    PL_hints = savehints;
    op_free(v);
}

#ifdef PERL_MAD
OP*
#else
void
#endif
Perl_utilize(pTHX_ int aver, I32 floor, OP *version, OP *idop, OP *arg)
{
    dVAR;
    OP *pack;
    OP *imop;
    OP *veop;
#ifdef PERL_MAD
    OP *pegop = newOP(OP_NULL,0);
#endif

    PERL_ARGS_ASSERT_UTILIZE;

    if (idop->op_type != OP_CONST)
	Perl_croak(aTHX_ "Module name must be constant");

    if (PL_madskills)
	op_getmad(idop,pegop,'U');

    veop = NULL;

    if (version) {
	SV * const vesv = ((SVOP*)version)->op_sv;

	if (PL_madskills)
	    op_getmad(version,pegop,'V');
	if (!arg && !SvNIOKp(vesv)) {
	    arg = version;
	}
	else {
	    OP *pack;
	    SV *meth;

	    if (version->op_type != OP_CONST || !SvNIOKp(vesv))
		Perl_croak(aTHX_ "Version number must be a constant number");

	    /* Make copy of idop so we don't free it twice */
	    pack = newSVOP(OP_CONST, 0, newSVsv(((SVOP*)idop)->op_sv));

	    /* Fake up a method call to VERSION */
	    meth = newSVpvs_share("VERSION");
	    veop = convert(OP_ENTERSUB, OPf_STACKED|OPf_SPECIAL,
			    append_elem(OP_LIST,
					prepend_elem(OP_LIST, pack, list(version)),
					newSVOP(OP_METHOD_NAMED, 0, meth)));
	}
    }

    /* Fake up an import/unimport */
    if (arg && arg->op_type == OP_STUB) {
	if (PL_madskills)
	    op_getmad(arg,pegop,'S');
	imop = arg;		/* no import on explicit () */
    }
    else if (SvNIOKp(((SVOP*)idop)->op_sv)) {
	imop = NULL;		/* use 5.0; */
	if (!aver)
	    idop->op_private |= OPpCONST_NOVER;
    }
    else {
	SV *meth;

	if (PL_madskills)
	    op_getmad(arg,pegop,'A');

	/* Make copy of idop so we don't free it twice */
	pack = newSVOP(OP_CONST, 0, newSVsv(((SVOP*)idop)->op_sv));

	/* Fake up a method call to import/unimport */
	meth = aver
	    ? newSVpvs_share("import") : newSVpvs_share("unimport");
	imop = convert(OP_ENTERSUB, OPf_STACKED|OPf_SPECIAL,
		       append_elem(OP_LIST,
				   prepend_elem(OP_LIST, pack, list(arg)),
				   newSVOP(OP_METHOD_NAMED, 0, meth)));
    }

    /* Fake up the BEGIN {}, which does its thing immediately. */
    newATTRSUB(floor,
	newSVOP(OP_CONST, 0, newSVpvs_share("BEGIN")),
	NULL,
	NULL,
	append_elem(OP_LINESEQ,
	    append_elem(OP_LINESEQ,
	        newSTATEOP(0, NULL, newUNOP(OP_REQUIRE, 0, idop)),
	        newSTATEOP(0, NULL, veop)),
	    newSTATEOP(0, NULL, imop) ));

    /* The "did you use incorrect case?" warning used to be here.
     * The problem is that on case-insensitive filesystems one
     * might get false positives for "use" (and "require"):
     * "use Strict" or "require CARP" will work.  This causes
     * portability problems for the script: in case-strict
     * filesystems the script will stop working.
     *
     * The "incorrect case" warning checked whether "use Foo"
     * imported "Foo" to your namespace, but that is wrong, too:
     * there is no requirement nor promise in the language that
     * a Foo.pm should or would contain anything in package "Foo".
     *
     * There is very little Configure-wise that can be done, either:
     * the case-sensitivity of the build filesystem of Perl does not
     * help in guessing the case-sensitivity of the runtime environment.
     */

    PL_hints |= HINT_BLOCK_SCOPE;
    PL_parser->copline = NOLINE;
    PL_parser->expect = XSTATE;
    PL_cop_seqmax++; /* Purely for B::*'s benefit */

#ifdef PERL_MAD
    if (!PL_madskills) {
	/* FIXME - don't allocate pegop if !PL_madskills */
	op_free(pegop);
	return NULL;
    }
    return pegop;
#endif
}

/*
=head1 Embedding Functions

=for apidoc load_module

Loads the module whose name is pointed to by the string part of name.
Note that the actual module name, not its filename, should be given.
Eg, "Foo::Bar" instead of "Foo/Bar.pm".  flags can be any of
PERL_LOADMOD_DENY, PERL_LOADMOD_NOIMPORT, or PERL_LOADMOD_IMPORT_OPS
(or 0 for no flags). ver, if specified, provides version semantics
similar to C<use Foo::Bar VERSION>.  The optional trailing SV*
arguments can be used to specify arguments to the module's import()
method, similar to C<use Foo::Bar VERSION LIST>.  They must be
terminated with a final NULL pointer.  Note that this list can only
be omitted when the PERL_LOADMOD_NOIMPORT flag has been used.
Otherwise at least a single NULL pointer to designate the default
import list is required.

=cut */

void
Perl_load_module(pTHX_ U32 flags, SV *name, SV *ver, ...)
{
    va_list args;

    PERL_ARGS_ASSERT_LOAD_MODULE;

    va_start(args, ver);
    vload_module(flags, name, ver, &args);
    va_end(args);
}

#ifdef PERL_IMPLICIT_CONTEXT
void
Perl_load_module_nocontext(U32 flags, SV *name, SV *ver, ...)
{
    dTHX;
    va_list args;
    PERL_ARGS_ASSERT_LOAD_MODULE_NOCONTEXT;
    va_start(args, ver);
    vload_module(flags, name, ver, &args);
    va_end(args);
}
#endif

void
Perl_vload_module(pTHX_ U32 flags, SV *name, SV *ver, va_list *args)
{
    dVAR;
    OP *veop, *imop;
    OP * const modname = newSVOP(OP_CONST, 0, name);

    PERL_ARGS_ASSERT_VLOAD_MODULE;

    modname->op_private |= OPpCONST_BARE;
    if (ver) {
	veop = newSVOP(OP_CONST, 0, ver);
    }
    else
	veop = NULL;
    if (flags & PERL_LOADMOD_NOIMPORT) {
	imop = sawparens(newNULLLIST());
    }
    else if (flags & PERL_LOADMOD_IMPORT_OPS) {
	imop = va_arg(*args, OP*);
    }
    else {
	SV *sv;
	imop = NULL;
	sv = va_arg(*args, SV*);
	while (sv) {
	    imop = append_elem(OP_LIST, imop, newSVOP(OP_CONST, 0, sv));
	    sv = va_arg(*args, SV*);
	}
    }

    /* utilize() fakes up a BEGIN { require ..; import ... }, so make sure
     * that it has a PL_parser to play with while doing that, and also
     * that it doesn't mess with any existing parser, by creating a tmp
     * new parser with lex_start(). This won't actually be used for much,
     * since pp_require() will create another parser for the real work. */

    ENTER;
    SAVEVPTR(PL_curcop);
    lex_start(NULL, NULL, FALSE);
    utilize(!(flags & PERL_LOADMOD_DENY), start_subparse(FALSE, 0),
	    veop, modname, imop);
    LEAVE;
}

OP *
Perl_dofile(pTHX_ OP *term, I32 force_builtin)
{
    dVAR;
    OP *doop;
    GV *gv = NULL;

    PERL_ARGS_ASSERT_DOFILE;

    if (!force_builtin) {
	gv = gv_fetchpvs("do", GV_NOTQUAL, SVt_PVCV);
	if (!(gv && GvCVu(gv) && GvIMPORTED_CV(gv))) {
	    GV * const * const gvp = (GV**)hv_fetchs(PL_globalstash, "do", FALSE);
	    gv = gvp ? *gvp : NULL;
	}
    }

    if (gv && GvCVu(gv) && GvIMPORTED_CV(gv)) {
	doop = ck_subr(convert(OP_ENTERSUB, OPf_STACKED,
			       append_elem(OP_LIST, term,
					   scalar(newUNOP(OP_RV2CV, 0,
							  newGVOP(OP_GV, 0, gv))))));
    }
    else {
	doop = newUNOP(OP_DOFILE, 0, scalar(term));
    }
    return doop;
}

/*
=head1 Optree construction

=for apidoc Am|OP *|newSLICEOP|I32 flags|OP *subscript|OP *listval

Constructs, checks, and returns an C<lslice> (list slice) op.  I<flags>
gives the eight bits of C<op_flags>, except that C<OPf_KIDS> will
be set automatically, and, shifted up eight bits, the eight bits of
C<op_private>, except that the bit with value 1 or 2 is automatically
set as required.  I<listval> and I<subscript> supply the parameters of
the slice; they are consumed by this function and become part of the
constructed op tree.

=cut
*/

OP *
Perl_newSLICEOP(pTHX_ I32 flags, OP *subscript, OP *listval)
{
    return newBINOP(OP_LSLICE, flags,
	    list(force_list(subscript)),
	    list(force_list(listval)) );
}

STATIC I32
S_is_list_assignment(pTHX_ register const OP *o)
{
    unsigned type;
    U8 flags;

    if (!o)
	return TRUE;

    if ((o->op_type == OP_NULL) && (o->op_flags & OPf_KIDS))
	o = cUNOPo->op_first;

    flags = o->op_flags;
    type = o->op_type;
    if (type == OP_COND_EXPR) {
        const I32 t = is_list_assignment(cLOGOPo->op_first->op_sibling);
        const I32 f = is_list_assignment(cLOGOPo->op_first->op_sibling->op_sibling);

	if (t && f)
	    return TRUE;
	if (t || f)
	    yyerror("Assignment to both a list and a scalar");
	return FALSE;
    }

    if (type == OP_LIST &&
	(flags & OPf_WANT) == OPf_WANT_SCALAR &&
	o->op_private & OPpLVAL_INTRO)
	return FALSE;

    if (type == OP_LIST || flags & OPf_PARENS ||
	type == OP_RV2AV || type == OP_RV2HV ||
	type == OP_ASLICE || type == OP_HSLICE)
	return TRUE;

    if (type == OP_PADAV || type == OP_PADHV)
	return TRUE;

    if (type == OP_RV2SV)
	return FALSE;

    return FALSE;
}

/*
  Helper function for newASSIGNOP to detection commonality between the
  lhs and the rhs.  Marks all variables with PL_generation.  If it
  returns TRUE the assignment must be able to handle common variables.
*/
bool
S_aassign_common_vars_left(pTHX_ OP* o)
{
    PERL_ARGS_ASSERT_AASSIGN_COMMON_VARS_LEFT;
    if (PL_opargs[o->op_type] & OA_DANGEROUS) {
	if (o->op_type == OP_GV) {
	    GV *gv = cGVOPx_gv(o);
	    if (gv == PL_defgv)
		return TRUE;
	    GvASSIGN_GENERATION_set(gv, PL_generation);
	}
	else if (o->op_type == OP_PADSV ||
	    o->op_type == OP_PADAV ||
	    o->op_type == OP_PADHV ||
	    o->op_type == OP_PADANY)
	    {
		PAD_COMPNAME_GEN_set(o->op_targ, PL_generation);
	    }
	else if (o->op_type == OP_RV2CV)
	    return FALSE;
	else if (o->op_type == OP_RV2SV ||
	    o->op_type == OP_RV2AV ||
	    o->op_type == OP_RV2HV ||
	    o->op_type == OP_RV2GV) {
	    if (cUNOPo->op_first->op_type != OP_GV)	/* funny deref? */
		return TRUE;
	}
    }
    if (o->op_flags & OPf_KIDS) {
	OP *kid;
	for (kid = cUNOPo->op_first; kid; kid=kid->op_sibling) {
	    if (aassign_common_vars_left(kid))
		return TRUE;
	}
    }
    return FALSE;
}

/*
  Helper function for newASSIGNOP to detection commonality between the
  lhs and the rhs.  Checks all variable used for marking with
  PL_generation, as privously set by aassign_common_vars_left If it
  returns TRUE the assignment must be able to handle common
  variables.
*/
bool
S_aassign_common_vars_right(pTHX_ OP* o)
{
    PERL_ARGS_ASSERT_AASSIGN_COMMON_VARS_RIGHT;
    if (PL_opargs[o->op_type] & OA_DANGEROUS) {
	if (o->op_type == OP_GV) {
	    GV *gv = cGVOPx_gv(o);
	    if (gv == PL_defgv
		|| (int)GvASSIGN_GENERATION(gv) == PL_generation)
		return TRUE;
	}
	else if (o->op_type == OP_PADSV ||
	    o->op_type == OP_PADAV ||
	    o->op_type == OP_PADHV ||
	    o->op_type == OP_PADANY)
	    {
		if (PAD_COMPNAME_GEN(o->op_targ)
		    == (STRLEN)PL_generation)
		    return TRUE;
	    }
	else if (o->op_type == OP_RV2CV)
	    return TRUE;
	else if (o->op_type == OP_RV2SV ||
	    o->op_type == OP_RV2AV ||
	    o->op_type == OP_RV2HV ||
	    o->op_type == OP_RV2GV) {
	    if (cUNOPo->op_first->op_type != OP_GV)	/* funny deref? */
		return TRUE;
	}
	else if (o->op_type == OP_PUSHRE) {
#ifdef USE_ITHREADS
	    if (((PMOP*)o)->op_pmreplrootu.op_pmtargetoff) {
		GV *const gv = MUTABLE_GV(PAD_SVl(((PMOP*)o)->op_pmreplrootu.op_pmtargetoff));
		if (gv == PL_defgv
		    || (int)GvASSIGN_GENERATION(gv) == PL_generation)
		    return TRUE;
	    }
#else
	    GV *const gv
		= ((PMOP*)o)->op_pmreplrootu.op_pmtargetgv;
	    if (gv) {
		if (gv == PL_defgv 
		    || (int)GvASSIGN_GENERATION(gv) == PL_generation)
		    return TRUE;
	    }
#endif
	}
	else
	    return TRUE;
    }
    if (o->op_flags & OPf_KIDS) {
	OP *kid;
	for (kid = cUNOPo->op_first; kid; kid=kid->op_sibling) {
	    if (aassign_common_vars_right(kid))
		return TRUE;
	}
    }
    return FALSE;
}

=for apidoc Am|OP *|newASSIGNOP|I32 flags|OP *left|I32 optype|OP *right

Constructs, checks, and returns an assignment op.  I<left> and I<right>
supply the parameters of the assignment; they are consumed by this
function and become part of the constructed op tree.

If I<optype> is C<OP_ANDASSIGN>, C<OP_ORASSIGN>, or C<OP_DORASSIGN>, then
a suitable conditional optree is constructed.  If I<optype> is the opcode
of a binary operator, such as C<OP_BIT_OR>, then an op is constructed that
performs the binary operation and assigns the result to the left argument.
Either way, if I<optype> is non-zero then I<flags> has no effect.

If I<optype> is zero, then a plain scalar or list assignment is
constructed.  Which type of assignment it is is automatically determined.
I<flags> gives the eight bits of C<op_flags>, except that C<OPf_KIDS>
will be set automatically, and, shifted up eight bits, the eight bits
of C<op_private>, except that the bit with value 1 or 2 is automatically
set as required.

=cut
*/
OP *
Perl_newASSIGNOP(pTHX_ I32 flags, OP *left, I32 optype, OP *right)
{
    dVAR;
    OP *o;

    if (optype) {
	if (optype == OP_ANDASSIGN || optype == OP_ORASSIGN || optype == OP_DORASSIGN) {
	    return newLOGOP(optype, 0,
		mod(scalar(left), optype),
		newUNOP(OP_SASSIGN, 0, scalar(right)));
	}
	else {
	    return newBINOP(optype, OPf_STACKED,
		mod(scalar(left), optype), scalar(right));
	}
    }

    if (is_list_assignment(left)) {
	static const char no_list_state[] = "Initialization of state variables"
	    " in list context currently forbidden";
	OP *curop;
	bool maybe_common_vars = TRUE;

	PL_modcount = 0;
	/* Grandfathering $[ assignment here.  Bletch.*/
	/* Only simple assignments like C<< ($[) = 1 >> are allowed */
	PL_eval_root = (left->op_type == OP_CONST) ? right : NULL;
	left = mod(left, OP_AASSIGN);
	if (left->op_type == OP_CONST) {
	    deprecate("assignment to $[");
	    /* FIXME for MAD */
	    /* Result of assignment is always 1 (or we'd be dead already) */
	    return newSVOP(OP_CONST, 0, newSViv(1));
	}
	curop = list(force_list(left));
	o = newBINOP(OP_AASSIGN, flags, list(force_list(right)), curop);
	o->op_private = (U8)(0 | (flags >> 8));

	if ((left->op_type == OP_LIST
	     || (left->op_type == OP_NULL && left->op_targ == OP_LIST)))
	{
	    OP* lop = ((LISTOP*)left)->op_first;
	    maybe_common_vars = FALSE;
	    while (lop) {
		if (lop->op_type == OP_PADSV ||
		    lop->op_type == OP_PADAV ||
		    lop->op_type == OP_PADHV ||
		    lop->op_type == OP_PADANY) {
		    if (!(lop->op_private & OPpLVAL_INTRO))
			maybe_common_vars = TRUE;

		    if (lop->op_private & OPpPAD_STATE) {
			if (left->op_private & OPpLVAL_INTRO) {
			    /* Each variable in state($a, $b, $c) = ... */
			}
			else {
			    /* Each state variable in
			       (state $a, my $b, our $c, $d, undef) = ... */
			}
			yyerror(no_list_state);
		    } else {
			/* Each my variable in
			   (state $a, my $b, our $c, $d, undef) = ... */
		    }
		} else if (lop->op_type == OP_UNDEF ||
			   lop->op_type == OP_PUSHMARK) {
		    /* undef may be interesting in
		       (state $a, undef, state $c) */
		} else {
		    /* Other ops in the list. */
		    maybe_common_vars = TRUE;
		}
		lop = lop->op_sibling;
	    }
	}
	else if ((left->op_private & OPpLVAL_INTRO)
		&& (   left->op_type == OP_PADSV
		    || left->op_type == OP_PADAV
		    || left->op_type == OP_PADHV
		    || left->op_type == OP_PADANY))
	{
	    if (left->op_type == OP_PADSV) maybe_common_vars = FALSE;
	    if (left->op_private & OPpPAD_STATE) {
		/* All single variable list context state assignments, hence
		   state ($a) = ...
		   (state $a) = ...
		   state @a = ...
		   state (@a) = ...
		   (state @a) = ...
		   state %a = ...
		   state (%a) = ...
		   (state %a) = ...
		*/
		yyerror(no_list_state);
	    }
	}

	/* PL_generation sorcery:
	 * an assignment like ($a,$b) = ($c,$d) is easier than
	 * ($a,$b) = ($c,$a), since there is no need for temporary vars.
	 * To detect whether there are common vars, the global var
	 * PL_generation is incremented for each assign op we compile.
	 * Then, while compiling the assign op, we run through all the
	 * variables on both sides of the assignment, setting a spare slot
	 * in each of them to PL_generation. If any of them already have
	 * that value, we know we've got commonality.  We could use a
	 * single bit marker, but then we'd have to make 2 passes, first
	 * to clear the flag, then to test and set it.  To find somewhere
	 * to store these values, evil chicanery is done with SvUVX().
	 */

	if (maybe_common_vars) {
	    PL_generation++;
	    if (aassign_common_vars_left(left))
		o->op_private |= OPpASSIGN_COMMON;
	    else if (aassign_common_vars_right(right))
		o->op_private |= OPpASSIGN_COMMON;
	}

	if (right && right->op_type == OP_SPLIT && !PL_madskills) {
	    OP* tmpop = ((LISTOP*)right)->op_first;
	    if (tmpop && (tmpop->op_type == OP_PUSHRE)) {
		PMOP * const pm = (PMOP*)tmpop;
		if (left->op_type == OP_RV2AV &&
		    !(left->op_private & OPpLVAL_INTRO) &&
		    !(o->op_private & OPpASSIGN_COMMON) )
		{
		    tmpop = ((UNOP*)left)->op_first;
		    if (tmpop->op_type == OP_GV
#ifdef USE_ITHREADS
			&& !pm->op_pmreplrootu.op_pmtargetoff
#else
			&& !pm->op_pmreplrootu.op_pmtargetgv
#endif
			) {
#ifdef USE_ITHREADS
			pm->op_pmreplrootu.op_pmtargetoff
			    = cPADOPx(tmpop)->op_padix;
			cPADOPx(tmpop)->op_padix = 0;	/* steal it */
#else
			pm->op_pmreplrootu.op_pmtargetgv
			    = MUTABLE_GV(cSVOPx(tmpop)->op_sv);
			cSVOPx(tmpop)->op_sv = NULL;	/* steal it */
#endif
			pm->op_pmflags |= PMf_ONCE;
			tmpop = cUNOPo->op_first;	/* to list (nulled) */
			cUNOPx(tmpop)->op_first = NULL;	/* don't free split */
			op_free(o);			/* blow off assign */
			right->op_flags &= ~OPf_WANT;
			right->op_context_known = FALSE;
				/* "I don't know and I don't care." */
			return right;
		    }
		}
		else {
                   if (PL_modcount < RETURN_UNLIMITED_NUMBER &&
		      ((LISTOP*)right)->op_last->op_type == OP_CONST)
		    {
			SV *sv = ((SVOP*)((LISTOP*)right)->op_last)->op_sv;
			if (SvIOK(sv) && SvIVX(sv) == 0)
			    sv_setiv(sv, PL_modcount+1);
		    }
		}
	    }
	}
	return o;
    }
    if (!right)
	right = newOP(OP_UNDEF, 0);
    if (right->op_type == OP_READLINE) {
	right->op_flags |= OPf_STACKED;
	return newBINOP(OP_NULL, flags, mod(scalar(left), OP_SASSIGN), scalar(right));
    }
    else {
	PL_eval_root = right;  /* Grandfathering $[ assignment here.  Bletch.*/
	o = newBINOP(OP_SASSIGN, flags,
	    scalar(right), mod(scalar(left), OP_SASSIGN) );
	if (PL_eval_root)
	    PL_eval_root = 0;
	else {
	    if (!PL_madskills) { /* assignment to $[ is ignored when making a mad dump */
		deprecate("assignment to $[");
		op_free(o);
		o = newSVOP(OP_CONST, 0, newSViv(CopARYBASE_get(&PL_compiling)));
		o->op_private |= OPpCONST_ARYBASE;
	    }
	}
    }

    return o;
}

/*
=for apidoc Am|OP *|newSTATEOP|I32 flags|char *label|OP *o

Constructs a state op (COP).  The state op is normally a C<nextstate> op,
but will be a C<dbstate> op if debugging is enabled for currently-compiled
code.  The state op is populated from L</PL_curcop> (or L</PL_compiling>).
If I<label> is non-null, it supplies the name of a label to attach to
the state op; this function takes ownership of the memory pointed at by
I<label>, and will free it.  I<flags> gives the eight bits of C<op_flags>
for the state op.

If I<o> is null, the state op is returned.  Otherwise the state op is
combined with I<o> into a C<lineseq> list op, which is returned.  I<o>
is consumed by this function and becomes part of the returned op tree.

=cut
*/

OP *
Perl_newSTATEOP(pTHX_ I32 flags, char *label, OP *o)
{
    dVAR;
    const U32 seq = intro_my();
    register COP *cop;

    NewOp(1101, cop, 1, COP);
    if (PERLDB_LINE && CopLINE(PL_curcop) && PL_curstash != PL_debstash) {
	cop->op_type = OP_DBSTATE;
    }
    else {
	cop->op_type = OP_NEXTSTATE;
    }
    cop->op_flags = (U8)flags;
    CopHINTS_set(cop, PL_hints);
#ifdef NATIVE_HINTS
    cop->op_private |= NATIVE_HINTS;
#endif
    CopHINTS_set(&PL_compiling, CopHINTS_get(cop));

    cop->cop_seq = seq;
    /* CopARYBASE is now "virtual", in that it's stored as a flag bit in
       CopHINTS and a possible value in cop_hints_hash, so no need to copy it.
    */
    cop->cop_warnings = DUP_WARNINGS(PL_curcop->cop_warnings);
    cop->cop_hints_hash = PL_curcop->cop_hints_hash;
    if (cop->cop_hints_hash) {
	HINTS_REFCNT_LOCK;
	cop->cop_hints_hash->refcounted_he_refcnt++;
	HINTS_REFCNT_UNLOCK;
    }
    if (label) {
	Perl_store_cop_label(aTHX_ cop, label, strlen(label), 0);
						     
	PL_hints |= HINT_BLOCK_SCOPE;
	/* It seems that we need to defer freeing this pointer, as other parts
	   of the grammar end up wanting to copy it after this op has been
	   created. */
	SAVEFREEPV(label);
    }

    if (PL_parser && PL_parser->copline == NOLINE)
        CopLINE_set(cop, CopLINE(PL_curcop));
    else {
	CopLINE_set(cop, PL_parser->copline);
	if (PL_parser)
	    PL_parser->copline = NOLINE;
    }
#ifdef USE_ITHREADS
    CopFILE_set(cop, CopFILE(PL_curcop));	/* XXX share in a pvtable? */
#else
    CopFILEGV_set(cop, CopFILEGV(PL_curcop));
#endif
    CopSTASH_set(cop, PL_curstash);

    if ((PERLDB_LINE || PERLDB_SAVESRC) && PL_curstash != PL_debstash) {
	/* this line can have a breakpoint - store the cop in IV */
	AV *av = CopFILEAVx(PL_curcop);
	if (av) {
	    SV * const * const svp = av_fetch(av, (I32)CopLINE(cop), FALSE);
	    if (svp && *svp != &PL_sv_undef ) {
		(void)SvIOK_on(*svp);
		SvIV_set(*svp, PTR2IV(cop));
	    }
	}
    }

    if (flags & OPf_SPECIAL)
	op_null((OP*)cop);
    return prepend_elem(OP_LINESEQ, (OP*)cop, o);
}

/*
=for apidoc Am|OP *|newLOGOP|I32 type|I32 flags|OP *first|OP *other

Constructs, checks, and returns a logical (flow control) op.  I<type>
is the opcode.  I<flags> gives the eight bits of C<op_flags>, except
that C<OPf_KIDS> will be set automatically, and, shifted up eight bits,
the eight bits of C<op_private>, except that the bit with value 1 is
automatically set.  I<first> supplies the expression controlling the
flow, and I<other> supplies the side (alternate) chain of ops; they are
consumed by this function and become part of the constructed op tree.

=cut
*/

OP *
Perl_newLOGOP(pTHX_ I32 type, I32 flags, OP *first, OP *other)
{
    dVAR;

    PERL_ARGS_ASSERT_NEWLOGOP;

    return new_logop(type, flags, &first, &other);
}

STATIC OP *
S_search_const(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_SEARCH_CONST;

    switch (o->op_type) {
	case OP_CONST:
	    return o;
	case OP_NULL:
	    if (o->op_flags & OPf_KIDS)
		return search_const(cUNOPo->op_first);
	    break;
	case OP_LEAVE:
	case OP_SCOPE:
	case OP_LINESEQ:
	{
	    OP *kid;
	    if (!(o->op_flags & OPf_KIDS))
		return NULL;
	    kid = cLISTOPo->op_first;
	    do {
		switch (kid->op_type) {
		    case OP_ENTER:
		    case OP_NULL:
		    case OP_NEXTSTATE:
			kid = kid->op_sibling;
			break;
		    default:
			if (kid != cLISTOPo->op_last)
			    return NULL;
			goto last;
		}
	    } while (kid);
	    if (!kid)
		kid = cLISTOPo->op_last;
last:
	    return search_const(kid);
	}
    }

    return NULL;
}

STATIC OP *
S_new_logop(pTHX_ I32 type, I32 flags, OP** firstp, OP** otherp)
{
    dVAR;
    LOGOP *logop;
    OP *first;
    OP *other;
    OP *cstop = NULL;
    OP *o;
    int prepend_not = 0;

    PERL_ARGS_ASSERT_NEW_LOGOP;

    first = *firstp;
    other = *otherp;

    if (type == OP_XOR)		/* Not short circuit, but here by precedence. */
	return newBINOP(type, flags, scalar(first), scalar(other));

    assert((PL_opargs[type] & OA_CLASS_MASK) == OA_LOGOP);

    scalarboolean(first);
    /* optimize AND and OR ops that have NOTs as children */
    if (first->op_type == OP_NOT
	&& (first->op_flags & OPf_KIDS)
	&& ((first->op_flags & OPf_SPECIAL) /* unless ($x) { } */
	    || (other->op_type == OP_NOT))  /* if (!$x && !$y) { } */
	&& !PL_madskills) {
	if (type == OP_AND || type == OP_OR) {
	    if (type == OP_AND)
		type = OP_OR;
	    else
		type = OP_AND;
	    op_null(first);
	    if (other->op_type == OP_NOT) { /* !a AND|OR !b => !(a OR|AND b) */
		op_null(other);
		prepend_not = 1; /* prepend a NOT op later */
	    }
	}
    }
    /* search for a constant op that could let us fold the test */
    if ((cstop = search_const(first))) {
	if (! ((type == OP_AND &&  SvTRUE(((SVOP*)cstop)->op_sv)) ||
		(type == OP_OR  && !SvTRUE(((SVOP*)cstop)->op_sv)) ||
		(type == OP_DOR && !SvOK(((SVOP*)cstop)->op_sv)))) {
	    /* check for C<my $x if 0>, or C<my($x,$y) if 0> */
	    const OP *o2 = other;
	    if ( ! (o2->op_type == OP_LIST
		    && (( o2 = cUNOPx(o2)->op_first)) )
	    )
		o2 = other;
	    if ((o2->op_type == OP_PADSV || o2->op_type == OP_PADAV
			|| o2->op_type == OP_PADHV)
		&& o2->op_private & OPpLVAL_INTRO
		&& !(o2->op_private & OPpPAD_STATE))
	    {
		Perl_ck_warner_d(aTHX_ packWARN(WARN_DEPRECATED),
				 "Deprecated use of my() in false conditional");
	    }
	}
    }
    else if ((first->op_flags & OPf_KIDS) && type != OP_DOR
	&& ckWARN(WARN_MISC)) /* [#24076] Don't warn for <FH> err FOO. */
    {
	const OP * const k1 = ((UNOP*)first)->op_first;
	const OP * const k2 = k1->op_sibling;
	OPCODE warnop = 0;
	switch (first->op_type)
	{
	case OP_NULL:
	    if (k2 && k2->op_type == OP_READLINE
		  && (k2->op_flags & OPf_STACKED)
		  && ((k1->op_flags & OPf_WANT) == OPf_WANT_SCALAR))
	    {
		warnop = k2->op_type;
	    }
	    break;

	case OP_SASSIGN:
	    if (k1->op_type == OP_READDIR
		  || k1->op_type == OP_GLOB
		  || (k1->op_type == OP_NULL && k1->op_targ == OP_GLOB)
		  || k1->op_type == OP_EACH)
	    {
		warnop = ((k1->op_type == OP_NULL)
			  ? (OPCODE)k1->op_targ : k1->op_type);
	    }
	    break;
	}
	if (warnop) {
	    const line_t oldline = CopLINE(PL_curcop);
	    CopLINE_set(PL_curcop, PL_parser->copline);
	    Perl_warner(aTHX_ packWARN(WARN_MISC),
		 "Value of %s%s can be \"0\"; test with defined()",
		 PL_op_desc[warnop],
		 ((warnop == OP_READLINE || warnop == OP_GLOB)
		  ? " construct" : "() operator"));
	    CopLINE_set(PL_curcop, oldline);
	}
    }

    if (!other)
	return first;

    if (type == OP_ANDASSIGN || type == OP_ORASSIGN || type == OP_DORASSIGN)
	other->op_private |= OPpASSIGN_BACKWARDS;  /* other is an OP_SASSIGN */

    NewOp(1101, logop, 1, LOGOP);

    logop->op_type = (OPCODE)type;
    logop->op_first = first;
    logop->op_flags = (U8)(flags | OPf_KIDS);
    logop->op_other_instr = NULL;
    logop->op_private = (U8)(1 | (flags >> 8));

    first->op_sibling = other;

    CHECKOP(type,logop);

    o = (OP*)logop;
    if (prepend_not)
	o = newUNOP(OP_NOT, 0, o);
    return o;
}

/*
=for apidoc Am|OP *|newCONDOP|I32 flags|OP *first|OP *trueop|OP *falseop

Constructs, checks, and returns a conditional-expression (C<cond_expr>)
op.  I<flags> gives the eight bits of C<op_flags>, except that C<OPf_KIDS>
will be set automatically, and, shifted up eight bits, the eight bits of
C<op_private>, except that the bit with value 1 is automatically set.
I<first> supplies the expression selecting between the two branches,
and I<trueop> and I<falseop> supply the branches; they are consumed by
this function and become part of the constructed op tree.

=cut
*/

OP *
Perl_newCONDOP(pTHX_ I32 flags, OP *first, OP *trueop, OP *falseop)
{
    dVAR;
    LOGOP *logop;

    PERL_ARGS_ASSERT_NEWCONDOP;

    if (!falseop)
	return newLOGOP(OP_AND, 0, first, trueop);
    if (!trueop)
	return newLOGOP(OP_OR, 0, first, falseop);

    scalarboolean(first);
    NewOp(1101, logop, 1, LOGOP);
    logop->op_type = OP_COND_EXPR;
    logop->op_first = first;
    logop->op_flags = (U8)(flags | OPf_KIDS);
    logop->op_private = (U8)(1 | (flags >> 8));

    CHECKOP(OP_COND_EXPR, /* that's logop->op_type */
	    logop);

    first->op_sibling = trueop;
    trueop->op_sibling = falseop;

    return (OP*)logop;
}

/*
=for apidoc Am|OP *|newRANGE|I32 flags|OP *left|OP *right

Constructs and returns a C<range> op, with subordinate C<flip> and
C<flop> ops.  I<flags> gives the eight bits of C<op_flags> for the
C<flip> op and, shifted up eight bits, the eight bits of C<op_private>
for both the C<flip> and C<range> ops, except that the bit with value
1 is automatically set.  I<left> and I<right> supply the expressions
controlling the endpoints of the range; they are consumed by this function
and become part of the constructed op tree.

=cut
*/

OP *
Perl_newRANGE(pTHX_ I32 flags, OP *left, OP *right)
{
    dVAR;
    LOGOP *range;
    OP *flip;

    PERL_ARGS_ASSERT_NEWRANGE;

    NewOp(1101, range, 1, LOGOP);

    flip = scalar(newUNOP(OP_FLIP, flags, left));

    range->op_type = OP_RANGE;
    range->op_first = flip;
    range->op_flags = OPf_KIDS | flags;
    range->op_private = (U8)(1 | (flags >> 8));

    left->op_sibling = right;

    range->op_targ = pad_alloc(OP_RANGE, SVs_PADMY);
    sv_upgrade(PAD_SV(range->op_targ), SVt_PVNV);
    flip->op_targ = pad_alloc(OP_RANGE, SVs_PADMY);
    sv_upgrade(PAD_SV(flip->op_targ), SVt_PVNV);

    flip->op_private =  left->op_type == OP_CONST ? OPpFLIP_LINENUM : 0;
    range->op_private = right->op_type == OP_CONST ? OPpFLIP_LINENUM : 0;

    return (OP*)range;
}

/*
=for apidoc Am|OP *|newLOOPOP|I32 flags|I32 debuggable|OP *expr|OP *block

Constructs, checks, and returns an op tree expressing a loop.  This is
only a loop in the control flow through the op tree; it does not have
the heavyweight loop structure that allows exiting the loop by C<last>
and suchlike.  I<flags> gives the eight bits of C<op_flags> for the
top-level op, except that some bits will be set automatically as required.
I<expr> supplies the expression controlling loop iteration, and I<block>
supplies the body of the loop; they are consumed by this function and
become part of the constructed op tree.  I<debuggable> is currently
unused and should always be 1.

=cut
*/

OP *
Perl_newLOOPOP(pTHX_ I32 flags, I32 debuggable, OP *expr, OP *block)
{
    dVAR;
    OP* listop;
    OP* o;
    const bool once = block && block->op_flags & OPf_SPECIAL &&
      (block->op_type == OP_ENTERSUB || block->op_type == OP_NULL);

    PERL_UNUSED_ARG(debuggable);

    if (expr) {
	if (once && expr->op_type == OP_CONST && !SvTRUE(((SVOP*)expr)->op_sv))
	    return block;	/* do {} while 0 does once */
	if (expr->op_type == OP_READLINE
	    || expr->op_type == OP_READDIR
	    || expr->op_type == OP_GLOB
	    || (expr->op_type == OP_NULL && expr->op_targ == OP_GLOB)) {
	    expr = newUNOP(OP_DEFINED, 0,
		newASSIGNOP(0, newDEFSVOP(), 0, expr) );
	} else if (expr->op_flags & OPf_KIDS) {
	    const OP * const k1 = ((UNOP*)expr)->op_first;
	    const OP * const k2 = k1 ? k1->op_sibling : NULL;
	    switch (expr->op_type) {
	      case OP_NULL:
		if (k2 && (k2->op_type == OP_READLINE || k2->op_type == OP_READDIR)
		      && (k2->op_flags & OPf_STACKED)
		      && ((k1->op_flags & OPf_WANT) == OPf_WANT_SCALAR))
		    expr = newUNOP(OP_DEFINED, 0, expr);
		break;

	      case OP_SASSIGN:
		if (k1 && (k1->op_type == OP_READDIR
		      || k1->op_type == OP_GLOB
		      || (k1->op_type == OP_NULL && k1->op_targ == OP_GLOB)
		      || k1->op_type == OP_EACH))
		    expr = newUNOP(OP_DEFINED, 0, expr);
		break;
	    }
	}
    }

    /* if block is null, the next append_elem() would put UNSTACK, a scalar
     * op, in listop. This is wrong. [perl #27024] */
    if (!block)
	block = newOP(OP_NULL, 0);
    listop = append_elem(OP_LINESEQ, block, NULL);
    o = new_logop(OP_WHILE_AND, 0, &expr, &listop);

    if (o == listop)
	o = newUNOP(OP_NULL, 0, o);	/* or do {} while 1 loses outer block */

    if (once)
	o->op_private |= OPpWHILE_AND_ONCE;

    o->op_flags |= flags;
    o = scope(o);
    o->op_flags |= OPf_SPECIAL;	/* suppress POPBLOCK curpm restoration*/
    return o;
}

/*
=for apidoc Am|OP *|newWHILEOP|I32 flags|I32 debuggable|LOOP *loop|I32 whileline|OP *expr|OP *block|OP *cont|I32 has_my

Constructs, checks, and returns an op tree expressing a C<while> loop.
This is a heavyweight loop, with structure that allows exiting the loop
by C<last> and suchlike.

I<loop> is an optional preconstructed C<enterloop> op to use in the
loop; if it is null then a suitable op will be constructed automatically.
I<expr> supplies the loop's controlling expression.  I<block> supplies the
main body of the loop, and I<cont> optionally supplies a C<continue> block
that operates as a second half of the body.  All of these optree inputs
are consumed by this function and become part of the constructed op tree.

I<flags> gives the eight bits of C<op_flags> for the C<leaveloop>
op and, shifted up eight bits, the eight bits of C<op_private> for
the C<leaveloop> op, except that (in both cases) some bits will be set
automatically.  I<debuggable> is currently unused and should always be 1.
I<whileline> is the line number that should be attributed to the loop's
controlling expression.  I<has_my> can be supplied as true to force the
loop body to be enclosed in its own scope.

=cut
*/

OP *
Perl_newWHILEOP(pTHX_ I32 flags, I32 debuggable, LOOP *loop, I32
whileline, OP *expr, OP *block, OP *cont, I32 has_my)
{
    dVAR;
    OP *redo;
    U8 loopflags = 0;

    PERL_UNUSED_ARG(debuggable);

    if (expr) {
	if (expr->op_type == OP_READLINE
         || expr->op_type == OP_READDIR
         || expr->op_type == OP_GLOB
		     || (expr->op_type == OP_NULL && expr->op_targ == OP_GLOB)) {
	    expr = newUNOP(OP_DEFINED, 0,
		newASSIGNOP(0, newDEFSVOP(), 0, expr) );
	} else if (expr->op_flags & OPf_KIDS) {
	    const OP * const k1 = ((UNOP*)expr)->op_first;
	    const OP * const k2 = (k1) ? k1->op_sibling : NULL;
	    switch (expr->op_type) {
	      case OP_NULL:
		if (k2 && (k2->op_type == OP_READLINE || k2->op_type == OP_READDIR)
		      && (k2->op_flags & OPf_STACKED)
		      && ((k1->op_flags & OPf_WANT) == OPf_WANT_SCALAR))
		    expr = newUNOP(OP_DEFINED, 0, expr);
		break;

	      case OP_SASSIGN:
		if (k1 && (k1->op_type == OP_READDIR
		      || k1->op_type == OP_GLOB
		      || (k1->op_type == OP_NULL && k1->op_targ == OP_GLOB)
		      || k1->op_type == OP_EACH))
		    expr = newUNOP(OP_DEFINED, 0, expr);
		break;
	    }
	}
    }

    if (!block)
	block = newOP(OP_NULL, 0);
    else if (cont || has_my) {
	block = scope(block);
    }

    if (expr) {
	OP * const unstack = newOP(OP_UNSTACK, 0);
	if (!cont)
	    cont = unstack;
	else
	    cont = append_elem(OP_LINESEQ, cont, unstack);
    }

    assert(block);
    redo = NULL;

    if (expr) {
    	PL_parser->copline = (line_t)whileline;
    }

    if (!expr)
	expr = newOP(OP_NOTHING, 0);
    else
	expr = scalar(expr);

    assert(!loop);
    NewOp(1101,loop,1,LOOP);
    loop->op_type = OP_ENTERLOOP;
    loop->op_private = 0;
    loop->op_flags = OPf_KIDS;
    
    loop->op_first = expr;
    loop->op_last = expr;

    append_elem(OP_ENTERLOOP, (OP*)loop, block);
    append_elem(OP_ENTERLOOP, (OP*)loop, cont);

    loop->op_private |= loopflags;

    loop->op_flags |= flags;
    loop->op_private |= (flags >> 8);
    return (OP*)loop;
}

/*
=for apidoc Am|OP *|newFOROP|I32 flags|char *label|line_t forline|OP *sv|OP *expr|OP *block|OP *cont

Constructs, checks, and returns an op tree expressing a C<foreach>
loop (iteration through a list of values).  This is a heavyweight loop,
with structure that allows exiting the loop by C<last> and suchlike.

I<sv> optionally supplies the variable that will be aliased to each
item in turn; if null, it defaults to C<$_> (either lexical or global).
I<expr> supplies the list of values to iterate over.  I<block> supplies
the main body of the loop, and I<cont> optionally supplies a C<continue>
block that operates as a second half of the body.  All of these optree
inputs are consumed by this function and become part of the constructed
op tree.

I<flags> gives the eight bits of C<op_flags> for the C<leaveloop>
op and, shifted up eight bits, the eight bits of C<op_private> for
the C<leaveloop> op, except that (in both cases) some bits will be set
automatically.  I<forline> is the line number that should be attributed
to the loop's list expression.  If I<label> is non-null, it supplies
the name of a label to attach to the state op at the start of the loop;
this function takes ownership of the memory pointed at by I<label>,
and will free it.

=cut
*/

OP *
Perl_newFOROP(pTHX_ I32 flags, char *label, line_t forline, OP *sv, OP *expr, OP *block, OP *cont)
{
    dVAR;
    LOOP *loop;
    PADOFFSET padoff = 0;
    I32 iterflags = 0;
    I32 iterpflags = 0;
    OP *madsv = NULL;
    PERL_UNUSED_ARG(flags);

    PERL_ARGS_ASSERT_NEWFOROP;

    if (sv) {
	if (sv->op_type == OP_RV2SV) {	/* symbol table variable */
	    iterpflags = sv->op_private & OPpOUR_INTRO; /* for our $x () */
	    sv->op_type = OP_RV2GV;

	    /* The op_type check is needed to prevent a possible segfault
	     * if the loop variable is undeclared and 'strict vars' is in
	     * effect. This is illegal but is nonetheless parsed, so we
	     * may reach this point with an OP_CONST where we're expecting
	     * an OP_GV.
	     */
	    if (cUNOPx(sv)->op_first->op_type == OP_GV
	     && cGVOPx_gv(cUNOPx(sv)->op_first) == PL_defgv)
		iterpflags |= OPpITER_DEF;
	}
	else if (sv->op_type == OP_PADSV) { /* private variable */
	    iterpflags = sv->op_private & OPpLVAL_INTRO; /* for my $x () */
	    padoff = sv->op_targ;
	    if (PL_madskills)
		madsv = sv;
	    else {
		sv->op_targ = 0;
		op_free(sv);
	    }
	    sv = NULL;
	}
	else
	    Perl_croak(aTHX_ "Can't use %s for loop variable", PL_op_desc[sv->op_type]);
	if (padoff) {
	    SV *const namesv = PAD_COMPNAME_SV(padoff);
	    STRLEN len;
	    const char *const name = SvPV_const(namesv, len);

	    if (len == 2 && name[0] == '$' && name[1] == '_')
		iterpflags |= OPpITER_DEF;
	}
    }
    else {
        const PADOFFSET offset = Perl_pad_findmy(aTHX_ STR_WITH_LEN("$_"), 0);
	if (offset == NOT_IN_PAD || PAD_COMPNAME_FLAGS_isOUR(offset)) {
	    sv = newGVOP(OP_GV, 0, PL_defgv);
	}
	else {
	    padoff = offset;
	}
	iterpflags |= OPpITER_DEF;
    }
    if (expr->op_type == OP_RV2AV || expr->op_type == OP_PADAV) {
	expr = mod(force_list(scalar(ref(expr, OP_ITER))), OP_GREPSTART);
	iterflags |= OPf_STACKED;
    }
    if (expr->op_type == OP_REVERSE) {
	if ((! cLISTOPx(expr)->op_first->op_sibling) &&
	    (cLISTOPx(expr)->op_first->op_type == OP_RV2AV || cLISTOPx(expr)->op_type == OP_PADAV)) {
	    cLISTOPx(expr)->op_first = mod(force_list(scalar(ref(cLISTOPx(expr)->op_first, OP_ITER))), OP_GREPSTART);
	    iterflags |= OPf_STACKED;
	}
	iterpflags |= OPpITER_REVERSED;
    }
    else if (expr->op_type == OP_REVERSE) {
    }
    else if (expr->op_type == OP_RANGE)
    {
	iterflags |= OPf_STACKED;
    }
    else {
        expr = mod(list(expr), OP_GREPSTART);
    }

    expr = list(expr);
    if (! sv)
	sv = newOP(OP_NOTHING, 0);
    sv = scalar(sv);

    NewOp(1101,loop,1,LOOP);
    loop->op_type = OP_FOREACH;
    loop->op_private = 0;
    loop->op_flags = OPf_KIDS | iterflags;

    loop->op_first = expr;
    loop->op_last = expr;

    append_elem(OP_FOREACH, (OP*)loop, sv);
    append_elem(OP_FOREACH, (OP*)loop, block);
    append_elem(OP_FOREACH, (OP*)loop, cont);

    /* prepend_elem(OP_FOREACH, loop, scalar(sv)); */
    /* for my  $x () sets OPpLVAL_INTRO;
     * for our $x () sets OPpOUR_INTRO */
    loop->op_private = (U8)iterpflags;
    loop->op_targ = padoff;
    if (madsv)
	op_getmad(madsv, (OP*)loop, 'v');
    PL_parser->copline = forline;
    return newSTATEOP(0, label, (OP*)loop);
}

/*
=for apidoc Am|OP *|newLOOPEX|I32 type|OP *label

Constructs, checks, and returns a loop-exiting op (such as C<goto>
or C<last>).  I<type> is the opcode.  I<label> supplies the parameter
determining the target of the op; it is consumed by this function and
become part of the constructed op tree.

=cut
*/

OP*
Perl_newLOOPEX(pTHX_ I32 type, OP *label)
{
    dVAR;
    OP *o;

    PERL_ARGS_ASSERT_NEWLOOPEX;

    assert((PL_opargs[type] & OA_CLASS_MASK) == OA_LOOPEXOP);

    if (type != OP_GOTO || label->op_type == OP_CONST) {
	/* "last()" means "last" */
	if (label->op_type == OP_STUB && (label->op_flags & OPf_PARENS))
	    o = newOP(type, OPf_SPECIAL);
	else {
	    o = newPVOP(type, 0, savesharedpv(label->op_type == OP_CONST
					? SvPV_nolen_const(((SVOP*)label)->op_sv)
					: ""));
	}
#ifdef PERL_MAD
	op_getmad(label,o,'L');
#else
	op_free(label);
#endif
    }
    else {
	/* Check whether it's going to be a goto &function */
	if (label->op_type == OP_ENTERSUB
		&& !(label->op_flags & OPf_STACKED))
	    label = newUNOP(OP_REFGEN, 0, mod(label, OP_REFGEN));
	o = newUNOP(type, OPf_STACKED, label);
    }
    PL_hints |= HINT_BLOCK_SCOPE;
    return o;
}

/* if the condition is a literal array or hash
   (or @{ ... } etc), make a reference to it.
 */
STATIC OP *
S_ref_array_or_hash(pTHX_ OP *cond)
{
    if (cond
    && (cond->op_type == OP_RV2AV
    ||  cond->op_type == OP_PADAV
    ||  cond->op_type == OP_RV2HV
    ||  cond->op_type == OP_PADHV))

	return newUNOP(OP_REFGEN,
	    0, mod(cond, OP_REFGEN));

    else
	return cond;
}

/* These construct the optree fragments representing given()
   and when() blocks.

   entergiven and enterwhen are LOGOPs; the op_other pointer
   points up to the associated leave op. We need this so we
   can put it in the context and make break/continue work.
   (Also, of course, pp_enterwhen will jump straight to
   op_other if the match fails.)
 */

STATIC OP *
S_newGIVWHENOP(pTHX_ OP *cond, OP *block,
		   I32 enter_opcode,
		   PADOFFSET entertarg)
{
    dVAR;
    LOGOP *enterop;

    PERL_ARGS_ASSERT_NEWGIVWHENOP;

    NewOp(1101, enterop, 1, LOGOP);
    enterop->op_type = (Optype)enter_opcode;
    enterop->op_flags =  (U8) OPf_KIDS;
    enterop->op_targ = ((entertarg == NOT_IN_PAD) ? 0 : entertarg);
    enterop->op_private = 0;

    if (cond) {
	cond = scalar(cond);
	enterop->op_first = cond;
	cond->op_sibling = block;
    }
    else {
	/* This is a default {} block */
	assert(enter_opcode == OP_ENTERWHEN);
	enterop->op_first = block;
	enterop->op_flags |= OPf_SPECIAL;
    }

    CHECKOP(enter_opcode, enterop); /* Currently does nothing, since
    				       entergiven and enterwhen both
    				       use ck_null() */

    return (OP*)enterop;
}

/* Does this look like a boolean operation? For these purposes
   a boolean operation is:
     - a subroutine call [*]
     - a logical connective
     - a comparison operator
     - a filetest operator, with the exception of -s -M -A -C
     - defined(), exists() or eof()
     - /$re/ or $foo =~ /$re/
   
   [*] possibly surprising
 */
STATIC bool
S_looks_like_bool(pTHX_ const OP *o)
{
    dVAR;

    PERL_ARGS_ASSERT_LOOKS_LIKE_BOOL;

    switch(o->op_type) {
	case OP_OR:
	case OP_DOR:
	    return looks_like_bool(cLOGOPo->op_first);

	case OP_AND:
	    return (
	    	looks_like_bool(cLOGOPo->op_first)
	     && looks_like_bool(cLOGOPo->op_first->op_sibling));

	case OP_NULL:
	case OP_SCALAR:
	    return (
		o->op_flags & OPf_KIDS
	    && looks_like_bool(cUNOPo->op_first));

	case OP_ENTERSUB:

	case OP_NOT:	case OP_XOR:

	case OP_EQ:	case OP_NE:	case OP_LT:
	case OP_GT:	case OP_LE:	case OP_GE:

	case OP_I_EQ:	case OP_I_NE:	case OP_I_LT:
	case OP_I_GT:	case OP_I_LE:	case OP_I_GE:

	case OP_SEQ:	case OP_SNE:	case OP_SLT:
	case OP_SGT:	case OP_SLE:	case OP_SGE:
	
	case OP_SMARTMATCH:
	
	case OP_FTRREAD:  case OP_FTRWRITE: case OP_FTREXEC:
	case OP_FTEREAD:  case OP_FTEWRITE: case OP_FTEEXEC:
	case OP_FTIS:     case OP_FTEOWNED: case OP_FTROWNED:
	case OP_FTZERO:   case OP_FTSOCK:   case OP_FTCHR:
	case OP_FTBLK:    case OP_FTFILE:   case OP_FTDIR:
	case OP_FTPIPE:   case OP_FTLINK:   case OP_FTSUID:
	case OP_FTSGID:   case OP_FTSVTX:   case OP_FTTTY:
	case OP_FTTEXT:   case OP_FTBINARY:
	
	case OP_DEFINED: case OP_EXISTS:
	case OP_MATCH:	 case OP_EOF:

	case OP_RANGE:

	    return TRUE;
	
	case OP_CONST:
	    /* Detect comparisons that have been optimized away */
	    if (cSVOPo->op_sv == &PL_sv_yes
	    ||  cSVOPo->op_sv == &PL_sv_no)
	    
		return TRUE;
	    else
		return FALSE;

	/* FALL THROUGH */
	default:
	    return FALSE;
    }
}

/*
=for apidoc Am|OP *|newGIVENOP|OP *cond|OP *block|PADOFFSET defsv_off

Constructs, checks, and returns an op tree expressing a C<given> block.
I<cond> supplies the expression that will be locally assigned to a lexical
variable, and I<block> supplies the body of the C<given> construct; they
are consumed by this function and become part of the constructed op tree.
I<defsv_off> is the pad offset of the scalar lexical variable that will
be affected.

=cut
*/

OP *
Perl_newGIVENOP(pTHX_ OP *cond, OP *block, PADOFFSET defsv_off)
{
    dVAR;
    PERL_ARGS_ASSERT_NEWGIVENOP;
    return newGIVWHENOP(
    	ref_array_or_hash(cond),
    	block,
	OP_ENTERGIVEN,
	defsv_off);
}

/*
=for apidoc Am|OP *|newWHENOP|OP *cond|OP *block

Constructs, checks, and returns an op tree expressing a C<when> block.
I<cond> supplies the test expression, and I<block> supplies the block
that will be executed if the test evaluates to true; they are consumed
by this function and become part of the constructed op tree.  I<cond>
will be interpreted DWIMically, often as a comparison against C<$_>,
and may be null to generate a C<default> block.

=cut
*/

OP *
Perl_newWHENOP(pTHX_ OP *cond, OP *block)
{
    const bool cond_llb = (!cond || looks_like_bool(cond));
    OP *cond_op;

    PERL_ARGS_ASSERT_NEWWHENOP;

    if (cond_llb)
	cond_op = cond;
    else {
	cond_op = newBINOP(OP_SMARTMATCH, OPf_SPECIAL,
		newDEFSVOP(),
		scalar(ref_array_or_hash(cond)));
    }
    
    return newGIVWHENOP(
	cond_op,
	    append_elem(block->op_type, block, newOP(OP_BREAK, OPf_SPECIAL)),
	OP_ENTERWHEN, 0);
}

/*
=head1 Embedding Functions

=for apidoc cv_undef

Clear out all the active components of a CV. This can happen either
by an explicit C<undef &foo>, or by the reference count going to zero.
In the former case, we keep the CvOUTSIDE pointer, so that any anonymous
children can still follow the full lexical scope chain.

=cut
*/

void
Perl_cv_undef(pTHX_ CV *cv)
{
    dVAR;

    PERL_ARGS_ASSERT_CV_UNDEF;

    DEBUG_X(PerlIO_printf(Perl_debug_log,
	  "CV undef: cv=0x%"UVxf" comppad=0x%"UVxf"\n",
	    PTR2UV(cv), PTR2UV(PL_comppad))
    );

#ifdef USE_ITHREADS
    if (CvFILE(cv) && !CvISXSUB(cv)) {
	/* for XSUBs CvFILE point directly to static memory; __FILE__ */
	Safefree(CvFILE(cv));
    }
    CvFILE(cv) = NULL;
#endif

    codeseq_refcnt_dec(CvCODESEQ(cv));

    if (!CvISXSUB(cv) && CvROOT(cv)) {
	if (SvTYPE(cv) == SVt_PVCV && CvDEPTH(cv))
	    Perl_croak(aTHX_ "Can't undef active subroutine");
	ENTER;

	PAD_SAVE_SETNULLPAD();

	op_free(CvROOT(cv));
	CvROOT(cv) = NULL;
	LEAVE;
    }
    SvPOK_off(MUTABLE_SV(cv));		/* forget prototype */
    CvGV_set(cv, NULL);

    pad_undef(cv);

    CvCODESEQ(cv) = NULL;

    /* remove CvOUTSIDE unless this is an undef rather than a free */
    if (!SvREFCNT(cv) && CvOUTSIDE(cv)) {
	if (!CvWEAKOUTSIDE(cv))
	    SvREFCNT_dec(CvOUTSIDE(cv));
	CvOUTSIDE(cv) = NULL;
    }
    if (CvCONST(cv)) {
	SvREFCNT_dec(MUTABLE_SV(CvXSUBANY(cv).any_ptr));
	CvCONST_off(cv);
    }
    if (CvISXSUB(cv) && CvXSUB(cv)) {
	CvXSUB(cv) = NULL;
    }
    /* delete all flags except WEAKOUTSIDE and CVGV_RC, which indicate the
     * ref status of CvOUTSIDE and CvGV */
    CvFLAGS(cv) &= (CVf_WEAKOUTSIDE|CVf_CVGV_RC);
}

void
Perl_cv_ckproto_len(pTHX_ const CV *cv, const GV *gv, const char *p,
		    const STRLEN len)
{
    PERL_ARGS_ASSERT_CV_CKPROTO_LEN;

    /* Can't just use a strcmp on the prototype, as CONSTSUBs "cheat" by
       relying on SvCUR, and doubling up the buffer to hold CvFILE().  */
    if (((!p != !SvPOK(cv)) /* One has prototype, one has not.  */
	 || (p && (len != SvCUR(cv) /* Not the same length.  */
		   || memNE(p, SvPVX_const(cv), len))))
	 && ckWARN_d(WARN_PROTOTYPE)) {
	SV* const msg = sv_newmortal();
	SV* name = NULL;

	if (gv)
	    gv_efullname3(name = sv_newmortal(), gv, NULL);
	sv_setpvs(msg, "Prototype mismatch:");
	if (name)
	    Perl_sv_catpvf(aTHX_ msg, " sub %"SVf, SVfARG(name));
	if (SvPOK(cv))
	    Perl_sv_catpvf(aTHX_ msg, " (%"SVf")", SVfARG(cv));
	else
	    sv_catpvs(msg, ": none");
	sv_catpvs(msg, " vs ");
	if (p)
	    Perl_sv_catpvf(aTHX_ msg, "(%.*s)", (int) len, p);
	else
	    sv_catpvs(msg, "none");
	Perl_warner(aTHX_ packWARN(WARN_PROTOTYPE), "%"SVf, SVfARG(msg));
    }
}

static void const_sv_xsub(pTHX_ CV* cv);

/*

=head1 Optree Manipulation Functions

=for apidoc cv_const_sv

If C<cv> is a constant sub eligible for inlining. returns the constant
value returned by the sub.  Otherwise, returns NULL.

Constant subs can be created with C<newCONSTSUB> or as described in
L<perlsub/"Constant Functions">.

=cut
*/
SV *
Perl_cv_const_sv(pTHX_ const CV *const cv)
{
    PERL_UNUSED_CONTEXT;
    if (!cv)
	return NULL;
    if (!(SvTYPE(cv) == SVt_PVCV || SvTYPE(cv) == SVt_PVFM))
	return NULL;
    return CvCONST(cv) ? MUTABLE_SV(CvXSUBANY(cv).any_ptr) : NULL;
}

/* op_const_sv:  examine an optree to determine whether it's in-lineable.
 * Can be called in 3 ways:
 *
 * !cv
 * 	look for a single OP_CONST with attached value: return the value
 *
 * cv && CvCLONE(cv) && !CvCONST(cv)
 *
 * 	examine the clone prototype, and if contains only a single
 * 	OP_CONST referencing a pad const, or a single PADSV referencing
 * 	an outer lexical, return a non-zero value to indicate the CV is
 * 	a candidate for "constizing" at clone time
 *
 * cv && CvCONST(cv)
 *
 *	We have just cloned an anon prototype that was marked as a const
 *	candidiate. Try to grab the current value, and in the case of
 *	PADSV, ignore it if it has multiple references. Return the value.
 */

SV *
Perl_op_const_sv(pTHX_ const OP *o, CV *cv)
{
    dVAR;
    SV *sv = NULL;

    if (PL_madskills)
	return NULL;

    if (!o)
	return NULL;

    if (o->op_type == OP_LINESEQ && cLISTOPo->op_first)
	o = cLISTOPo->op_first->op_sibling;

    for (; o; o = o->op_sibling) {
    	const OPCODE type = o->op_type;

	if (type == OP_NEXTSTATE || type == OP_NULL || type == OP_PUSHMARK)
	    continue;
	if (type == OP_DBSTATE)
	    continue;
    	if (type == OP_LEAVESUB || type == OP_RETURN)
    	    break;
    	if (sv)
    	    return NULL;
    	if (type == OP_CONST && cSVOPo->op_sv)
    	    sv = cSVOPo->op_sv;
    	else if (cv && type == OP_CONST) {
    	    sv = PAD_BASE_SV(CvPADLIST(cv), o->op_targ);
    	    if (!sv)
    		return NULL;
    	}
    	else if (cv && type == OP_PADSV) {
    	    if (CvCONST(cv)) { /* newly cloned anon */
    		sv = PAD_BASE_SV(CvPADLIST(cv), o->op_targ);
    		/* the candidate should have 1 ref from this pad and 1 ref
    		 * from the parent */
    		if (!sv || SvREFCNT(sv) != 2)
    		    return NULL;
    		sv = newSVsv(sv);
    		SvREADONLY_on(sv);
    		return sv;
    	    }
    	    else {
    		if (PAD_COMPNAME_FLAGS(o->op_targ) & SVf_FAKE)
    		    sv = &PL_sv_undef; /* an arbitrary non-null value */
    	    }
    	}
    	else {
    	    return NULL;
    	}
    }
    return sv;
}

#ifdef PERL_MAD
OP *
#else
void
#endif
Perl_newMYSUB(pTHX_ I32 floor, OP *o, OP *proto, OP *attrs, OP *block)
{
#if 0
    /* This would be the return value, but the return cannot be reached.  */
    OP* pegop = newOP(OP_NULL, 0);
#endif

    PERL_UNUSED_ARG(floor);

    if (o)
	SAVEFREEOP(o);
    if (proto)
	SAVEFREEOP(proto);
    if (attrs)
	SAVEFREEOP(attrs);
    if (block)
	SAVEFREEOP(block);
    Perl_croak(aTHX_ "\"my sub\" not yet implemented");
#ifdef PERL_MAD
    NORETURN_FUNCTION_END;
#endif
}

CV *
Perl_newSUB(pTHX_ I32 floor, OP *o, OP *proto, OP *block)
{
    return Perl_newATTRSUB(aTHX_ floor, o, proto, NULL, block);
}

CV *
Perl_newATTRSUB(pTHX_ I32 floor, OP *o, OP *proto, OP *attrs, OP *block)
{
    dVAR;
    GV *gv;
    const char *ps;
    STRLEN ps_len = 0; /* init it to avoid false uninit warning from icc */
    register CV *cv = NULL;
    SV *const_sv;
    /* If the subroutine has no body, no attributes, and no builtin attributes
       then it's just a sub declaration, and we may be able to get away with
       storing with a placeholder scalar in the symbol table, rather than a
       full GV and CV.  If anything is present then it will take a full CV to
       store it.  */
    const I32 gv_fetch_flags
	= (block || attrs || (CvFLAGS(PL_compcv) & CVf_BUILTIN_ATTRS)
	   || PL_madskills)
	? GV_ADDMULTI : GV_ADDMULTI | GV_NOINIT;
    const char * const name = o ? SvPV_nolen_const(cSVOPo->op_sv) : NULL;
    bool has_name;

    if (proto) {
	assert(proto->op_type == OP_CONST);
	ps = SvPV_const(((SVOP*)proto)->op_sv, ps_len);
    }
    else
	ps = NULL;

    if (name) {
	gv = gv_fetchsv(cSVOPo->op_sv, gv_fetch_flags, SVt_PVCV);
	has_name = TRUE;
    } else if (PERLDB_NAMEANON && CopLINE(PL_curcop)) {
	SV * const sv = sv_newmortal();
	Perl_sv_setpvf(aTHX_ sv, "%s[%s:%"IVdf"]",
		       PL_curstash ? "__ANON__" : "__ANON__::__ANON__",
		       CopFILE(PL_curcop), (IV)CopLINE(PL_curcop));
	gv = gv_fetchsv(sv, gv_fetch_flags, SVt_PVCV);
	has_name = TRUE;
    } else if (PL_curstash) {
	gv = gv_fetchpvs("__ANON__", gv_fetch_flags, SVt_PVCV);
	has_name = FALSE;
    } else {
	gv = gv_fetchpvs("__ANON__::__ANON__", gv_fetch_flags, SVt_PVCV);
	has_name = FALSE;
    }

    if (!PL_madskills) {
	if (o)
	    SAVEFREEOP(o);
	if (proto)
	    SAVEFREEOP(proto);
	if (attrs)
	    SAVEFREEOP(attrs);
    }

    if (SvTYPE(gv) != SVt_PVGV) {	/* Maybe prototype now, and had at
					   maximum a prototype before. */
	if (SvTYPE(gv) > SVt_NULL) {
	    if (!SvPOK((const SV *)gv)
		&& !(SvIOK((const SV *)gv) && SvIVX((const SV *)gv) == -1))
	    {
		Perl_ck_warner_d(aTHX_ packWARN(WARN_PROTOTYPE), "Runaway prototype");
	    }
	    cv_ckproto_len((const CV *)gv, NULL, ps, ps_len);
	}
	if (ps)
	    sv_setpvn(MUTABLE_SV(gv), ps, ps_len);
	else
	    sv_setiv(MUTABLE_SV(gv), -1);

	SvREFCNT_dec(PL_compcv);
	cv = PL_compcv = NULL;
	goto done;
    }

    cv = (!name || GvCVGEN(gv)) ? NULL : GvCV(gv);

    if (!block || !ps || *ps || attrs
	|| (CvFLAGS(PL_compcv) & CVf_BUILTIN_ATTRS)
#ifdef PERL_MAD
	|| block->op_type == OP_NULL
#endif
	)
	const_sv = NULL;
    else
	const_sv = op_const_sv(block, NULL);

    if (cv) {
        const bool exists = CvROOT(cv) || CvXSUB(cv);

        /* if the subroutine doesn't exist and wasn't pre-declared
         * with a prototype, assume it will be AUTOLOADed,
         * skipping the prototype check
         */
        if (exists || SvPOK(cv))
	    cv_ckproto_len(cv, gv, ps, ps_len);
	/* already defined (or promised)? */
	if (exists || GvASSUMECV(gv)) {
	    if ((!block
#ifdef PERL_MAD
		 || block->op_type == OP_NULL
#endif
		 )&& !attrs) {
		if (CvFLAGS(PL_compcv)) {
		    /* might have had built-in attrs applied */
		    if (CvLVALUE(PL_compcv) && ! CvLVALUE(cv) && ckWARN(WARN_MISC))
			Perl_warner(aTHX_ packWARN(WARN_MISC), "lvalue attribute ignored after the subroutine has been defined");
		    CvFLAGS(cv) |= (CvFLAGS(PL_compcv) & CVf_BUILTIN_ATTRS & ~CVf_LVALUE);
		}
		/* just a "sub foo;" when &foo is already defined */
		SAVEFREESV(PL_compcv);
		goto done;
	    }
	    if (block
#ifdef PERL_MAD
		&& block->op_type != OP_NULL
#endif
		) {
		if (ckWARN(WARN_REDEFINE)
		    || (CvCONST(cv)
			&& (!const_sv || sv_cmp(cv_const_sv(cv), const_sv))))
		{
		    const line_t oldline = CopLINE(PL_curcop);
		    if (PL_parser && PL_parser->copline != NOLINE)
			CopLINE_set(PL_curcop, PL_parser->copline);
		    Perl_warner(aTHX_ packWARN(WARN_REDEFINE),
			CvCONST(cv) ? "Constant subroutine %s redefined"
				    : "Subroutine %s redefined", name);
		    CopLINE_set(PL_curcop, oldline);
		}
#ifdef PERL_MAD
		if (!PL_minus_c)	/* keep old one around for madskills */
#endif
		    {
			/* (PL_madskills unset in used file.) */
			SvREFCNT_dec(cv);
		    }
		cv = NULL;
	    }
	}
    }
    if (const_sv) {
	SvREFCNT_inc_simple_void_NN(const_sv);
	if (cv) {
	    assert(!CvROOT(cv) && !CvCONST(cv));
	    sv_setpvs(MUTABLE_SV(cv), "");  /* prototype is "" */
	    CvXSUBANY(cv).any_ptr = const_sv;
	    CvXSUB(cv) = const_sv_xsub;
	    CvCONST_on(cv);
	    CvISXSUB_on(cv);
	}
	else {
	    GvCV(gv) = NULL;
	    cv = newCONSTSUB(NULL, name, const_sv);
	}
        mro_method_changed_in( /* sub Foo::Bar () { 123 } */
            (CvGV(cv) && GvSTASH(CvGV(cv)))
                ? GvSTASH(CvGV(cv))
                : CvSTASH(cv)
                    ? CvSTASH(cv)
                    : PL_curstash
        );
	if (PL_madskills)
	    goto install_block;
	op_free(block);
	SvREFCNT_dec(PL_compcv);
	PL_compcv = NULL;
	goto done;
    }
    if (cv) {				/* must reuse cv if autoloaded */
	/* transfer PL_compcv to cv */
	if (block
#ifdef PERL_MAD
                  && block->op_type != OP_NULL
#endif
	) {
	    cv_flags_t existing_builtin_attrs = CvFLAGS(cv) & CVf_BUILTIN_ATTRS;
	    cv_undef(cv);
	    CvFLAGS(cv) = CvFLAGS(PL_compcv) | existing_builtin_attrs;
	    if (!CvWEAKOUTSIDE(cv))
		SvREFCNT_dec(CvOUTSIDE(cv));
	    CvOUTSIDE(cv) = CvOUTSIDE(PL_compcv);
	    CvOUTSIDE_SEQ(cv) = CvOUTSIDE_SEQ(PL_compcv);
	    CvOUTSIDE(PL_compcv) = 0;
	    CvPADLIST(cv) = CvPADLIST(PL_compcv);
	    CvPADLIST(PL_compcv) = 0;
	    /* inner references to PL_compcv must be fixed up ... */
	    pad_fixup_inner_anons(CvPADLIST(cv), PL_compcv, cv);
	    if (PERLDB_INTER)/* Advice debugger on the new sub. */
	      ++PL_sub_generation;
	    if (CvSTASH(cv))
		sv_del_backref(MUTABLE_SV(CvSTASH(cv)), MUTABLE_SV(cv));
	}
	else {
	    /* Might have had built-in attributes applied -- propagate them. */
	    CvFLAGS(cv) |= (CvFLAGS(PL_compcv) & CVf_BUILTIN_ATTRS);
	}
	/* ... before we throw it away */
	SvREFCNT_dec(PL_compcv);
	PL_compcv = cv;
    }
    else {
	cv = PL_compcv;
	if (name) {
	    GvCV(gv) = cv;
	    if (PL_madskills) {
		if (strEQ(name, "import")) {
		    PL_formfeed = MUTABLE_SV(cv);
		    /* diag_listed_as: SKIPME */
		    Perl_warner(aTHX_ packWARN(WARN_VOID), "0x%"UVxf"\n", PTR2UV(cv));
		}
	    }
	    GvCVGEN(gv) = 0;
            mro_method_changed_in(GvSTASH(gv)); /* sub Foo::bar { (shift)+1 } */
	}
    }
    if (!CvGV(cv)) {
	CvGV_set(cv, gv);
	CvFILE_set_from_cop(cv, PL_curcop);
	CvSTASH(cv) = PL_curstash;
	if (PL_curstash)
	    Perl_sv_add_backref(aTHX_ MUTABLE_SV(PL_curstash), MUTABLE_SV(cv));
    }
    if (attrs) {
	/* Need to do a C<use attributes $stash_of_cv,\&cv,@attrs>. */
	HV *stash = name && GvSTASH(CvGV(cv)) ? GvSTASH(CvGV(cv)) : PL_curstash;
	apply_attrs(stash, MUTABLE_SV(cv), attrs, FALSE);
    }

    if (ps)
	sv_setpvn(MUTABLE_SV(cv), ps, ps_len);

    if (PL_parser && PL_parser->error_count) {
	op_free(block);
	block = NULL;
	if (name) {
	    const char *s = strrchr(name, ':');
	    s = s ? s+1 : name;
	    if (strEQ(s, "BEGIN")) {
		const char not_safe[] =
		    "BEGIN not safe after errors--compilation aborted";
		if (PL_in_eval & EVAL_KEEPERR)
		    Perl_croak(aTHX_ not_safe);
		else {
		    /* force display of errors found but not reported */
		    sv_catpv(ERRSV, not_safe);
		    Perl_croak(aTHX_ "%"SVf, SVfARG(ERRSV));
		}
	    }
	}
    }
 install_block:
    if (!block)
	goto done;

    /* If we assign an optree to a PVCV, then we've defined a subroutine that
       the debugger could be able to set a breakpoint in, so signal to
       pp_entereval that it should not throw away any saved lines at scope
       exit.  */
       
    PL_breakable_sub_gen++;
    if (CvLVALUE(cv)) {
	CvROOT(cv) = newUNOP(OP_LEAVESUBLV, 0,
			     mod(scalarseq(block), OP_LEAVESUBLV));
	block->op_attached = 1;
    }
    else {
	/* This makes sub {}; work as expected.  */
	if (block->op_type == OP_STUB) {
	    OP* const newblock = newSTATEOP(0, NULL, 0);
#ifdef PERL_MAD
	    op_getmad(block,newblock,'B');
#else
	    op_free(block);
#endif
	    block = newblock;
	}
	else
	    block->op_attached = 1;
	CvROOT(cv) = newUNOP(OP_LEAVESUB, 0, scalarseq(block));
    }
    CvROOT(cv) = S_unknown(CvROOT(cv));
    CvROOT(cv)->op_private |= OPpREFCOUNTED;
    OpREFCNT_set(CvROOT(cv), 1);
    finish_optree(CvROOT(cv));

    /* now that optimizer has done its work, adjust pad values */

    pad_tidy(CvCLONE(cv) ? padtidy_SUBCLONE : padtidy_SUB);

    if (CvCLONE(cv)) {
	assert(!CvCONST(cv));
	if (ps && !*ps && op_const_sv(block, cv))
	    CvCONST_on(cv);
    }

    if (has_name) {
	if (PERLDB_SUBLINE && PL_curstash != PL_debstash) {
	    SV * const tmpstr = sv_newmortal();
	    GV * const db_postponed = gv_fetchpvs("DB::postponed",
						  GV_ADDMULTI, SVt_PVHV);
	    HV *hv;
	    SV * const sv = Perl_newSVpvf(aTHX_ "%s:%ld-%ld",
					  CopFILE(PL_curcop),
					  (long)PL_subline,
					  (long)CopLINE(PL_curcop));
	    gv_efullname3(tmpstr, gv, NULL);
	    (void)hv_store(GvHV(PL_DBsub), SvPVX_const(tmpstr),
		    SvCUR(tmpstr), sv, 0);
	    hv = GvHVn(db_postponed);
	    if (HvTOTALKEYS(hv) > 0 && hv_exists(hv, SvPVX_const(tmpstr), SvCUR(tmpstr))) {
		CV * const pcv = GvCV(db_postponed);
		if (pcv) {
		    dSP;
		    PUSHMARK(SP);
		    XPUSHs(tmpstr);
		    PUTBACK;
		    call_sv(MUTABLE_SV(pcv), G_DISCARD);
		}
	    }
	}

	if (name && ! (PL_parser && PL_parser->error_count))
	    process_special_blocks(name, gv, cv);
    }

  done:
    if (PL_parser)
	PL_parser->copline = NOLINE;
    LEAVE_SCOPE(floor);
    return cv;
}

STATIC void
S_process_special_blocks(pTHX_ const char *const fullname, GV *const gv,
			 CV *const cv)
{
    const char *const colon = strrchr(fullname,':');
    const char *const name = colon ? colon + 1 : fullname;

    PERL_ARGS_ASSERT_PROCESS_SPECIAL_BLOCKS;

    if (*name == 'B') {
	if (strEQ(name, "BEGIN")) {
	    const I32 oldscope = PL_scopestack_ix;
	    ENTER;
	    SAVECOPFILE(&PL_compiling);
	    SAVECOPLINE(&PL_compiling);

	    DEBUG_x( dump_sub(gv) );
	    Perl_av_create_and_push(aTHX_ &PL_beginav, MUTABLE_SV(cv));
	    GvCV(gv) = 0;		/* cv has been hijacked */
	    call_list(oldscope, PL_beginav);

	    PL_curcop = &PL_compiling;
	    CopHINTS_set(&PL_compiling, PL_hints);
	    LEAVE;
	}
	else
	    return;
    } else {
	if (*name == 'E') {
	    if strEQ(name, "END") {
		DEBUG_x( dump_sub(gv) );
		Perl_av_create_and_unshift_one(aTHX_ &PL_endav, MUTABLE_SV(cv));
	    } else
		return;
	} else if (*name == 'U') {
	    if (strEQ(name, "UNITCHECK")) {
		/* It's never too late to run a unitcheck block */
		Perl_av_create_and_unshift_one(aTHX_ &PL_unitcheckav, MUTABLE_SV(cv));
	    }
	    else
		return;
	} else if (*name == 'C') {
	    if (strEQ(name, "CHECK")) {
		if (PL_main_root)
		    Perl_ck_warner(aTHX_ packWARN(WARN_VOID),
				   "Too late to run CHECK block");
		Perl_av_create_and_unshift_one(aTHX_ &PL_checkav, MUTABLE_SV(cv));
	    }
	    else
		return;
	} else if (*name == 'I') {
	    if (strEQ(name, "INIT")) {
		if (PL_main_root)
		    Perl_ck_warner(aTHX_ packWARN(WARN_VOID),
				   "Too late to run INIT block");
		Perl_av_create_and_push(aTHX_ &PL_initav, MUTABLE_SV(cv));
	    }
	    else
		return;
	} else
	    return;
	DEBUG_x( dump_sub(gv) );
	GvCV(gv) = 0;		/* cv has been hijacked */
    }
}

/*
=for apidoc newCONSTSUB

Creates a constant sub equivalent to Perl C<sub FOO () { 123 }> which is
eligible for inlining at compile-time.

Passing NULL for SV creates a constant sub equivalent to C<sub BAR () {}>,
which won't be called if used as a destructor, but will suppress the overhead
of a call to C<AUTOLOAD>.  (This form, however, isn't eligible for inlining at
compile time.)

=cut
*/

CV *
Perl_newCONSTSUB(pTHX_ HV *stash, const char *name, SV *sv)
{
    dVAR;
    CV* cv;
#ifdef USE_ITHREADS
    const char *const file = CopFILE(PL_curcop);
#else
    SV *const temp_sv = CopFILESV(PL_curcop);
    const char *const file = temp_sv ? SvPV_nolen_const(temp_sv) : NULL;
#endif

    ENTER;

    if (IN_PERL_RUNTIME) {
	/* at runtime, it's not safe to manipulate PL_curcop: it may be
	 * an op shared between threads. Use a non-shared COP for our
	 * dirty work */
	 SAVEVPTR(PL_curcop);
	 PL_curcop = &PL_compiling;
    }
    SAVECOPLINE(PL_curcop);
    CopLINE_set(PL_curcop, PL_parser ? PL_parser->copline : NOLINE);

    SAVEHINTS();
    PL_hints &= ~HINT_BLOCK_SCOPE;

    if (stash) {
	SAVESPTR(PL_curstash);
	SAVECOPSTASH(PL_curcop);
	PL_curstash = stash;
	CopSTASH_set(PL_curcop,stash);
    }

    /* file becomes the CvFILE. For an XS, it's supposed to be static storage,
       and so doesn't get free()d.  (It's expected to be from the C pre-
       processor __FILE__ directive). But we need a dynamically allocated one,
       and we need it to get freed.  */
    cv = newXS_flags(name, const_sv_xsub, file ? file : "", "",
		     XS_DYNAMIC_FILENAME);
    CvXSUBANY(cv).any_ptr = sv;
    CvCONST_on(cv);

#ifdef USE_ITHREADS
    if (stash)
	CopSTASH_free(PL_curcop);
#endif
    LEAVE;

    return cv;
}

CV *
Perl_newXS_flags(pTHX_ const char *name, XSUBADDR_t subaddr,
		 const char *const filename, const char *const proto,
		 U32 flags)
{
    CV *cv = newXS(name, subaddr, filename);

    PERL_ARGS_ASSERT_NEWXS_FLAGS;

    if (flags & XS_DYNAMIC_FILENAME) {
	/* We need to "make arrangements" (ie cheat) to ensure that the
	   filename lasts as long as the PVCV we just created, but also doesn't
	   leak  */
	STRLEN filename_len = strlen(filename);
	STRLEN proto_and_file_len = filename_len;
	char *proto_and_file;
	STRLEN proto_len;

	if (proto) {
	    proto_len = strlen(proto);
	    proto_and_file_len += proto_len;

	    Newx(proto_and_file, proto_and_file_len + 1, char);
	    Copy(proto, proto_and_file, proto_len, char);
	    Copy(filename, proto_and_file + proto_len, filename_len + 1, char);
	} else {
	    proto_len = 0;
	    proto_and_file = savepvn(filename, filename_len);
	}

	/* This gets free()d.  :-)  */
	sv_usepvn_flags(MUTABLE_SV(cv), proto_and_file, proto_and_file_len,
			SV_HAS_TRAILING_NUL);
	if (proto) {
	    /* This gives us the correct prototype, rather than one with the
	       file name appended.  */
	    SvCUR_set(cv, proto_len);
	} else {
	    SvPOK_off(cv);
	}
	CvFILE(cv) = proto_and_file + proto_len;
    } else {
	sv_setpv(MUTABLE_SV(cv), proto);
    }
    return cv;
}

/*
=for apidoc U||newXS

Used by C<xsubpp> to hook up XSUBs as Perl subs.  I<filename> needs to be
static storage, as it is used directly as CvFILE(), without a copy being made.

=cut
*/

CV *
Perl_newXS(pTHX_ const char *name, XSUBADDR_t subaddr, const char *filename)
{
    dVAR;
    GV * const gv = gv_fetchpv(name ? name :
			(PL_curstash ? "__ANON__" : "__ANON__::__ANON__"),
			GV_ADDMULTI, SVt_PVCV);
    register CV *cv;

    PERL_ARGS_ASSERT_NEWXS;

    if (!subaddr)
	Perl_croak(aTHX_ "panic: no address for '%s' in '%s'", name, filename);

    if ((cv = (name ? GvCV(gv) : NULL))) {
	if (GvCVGEN(gv)) {
	    /* just a cached method */
	    SvREFCNT_dec(cv);
	    cv = NULL;
	}
	else if (CvROOT(cv) || CvXSUB(cv) || GvASSUMECV(gv)) {
	    /* already defined (or promised) */
	    /* XXX It's possible for this HvNAME_get to return null, and get passed into strEQ */
	    if (ckWARN(WARN_REDEFINE)) {
		GV * const gvcv = CvGV(cv);
		if (gvcv) {
		    HV * const stash = GvSTASH(gvcv);
		    if (stash) {
			const char *redefined_name = HvNAME_get(stash);
			if ( strEQ(redefined_name,"autouse") ) {
			    const line_t oldline = CopLINE(PL_curcop);
			    if (PL_parser && PL_parser->copline != NOLINE)
				CopLINE_set(PL_curcop, PL_parser->copline);
			    Perl_warner(aTHX_ packWARN(WARN_REDEFINE),
					CvCONST(cv) ? "Constant subroutine %s redefined"
						    : "Subroutine %s redefined"
					,name);
			    CopLINE_set(PL_curcop, oldline);
			}
		    }
		}
	    }
	    SvREFCNT_dec(cv);
	    cv = NULL;
	}
    }

    if (cv)				/* must reuse cv if autoloaded */
	cv_undef(cv);
    else {
	cv = MUTABLE_CV(newSV_type(SVt_PVCV));
	if (name) {
	    GvCV(gv) = cv;
	    GvCVGEN(gv) = 0;
            mro_method_changed_in(GvSTASH(gv)); /* newXS */
	}
    }
    if (!name)
	CvANON_on(cv);
    CvGV_set(cv, gv);
    (void)gv_fetchfile(filename);
    CvFILE(cv) = (char *)filename; /* NOTE: not copied, as it is expected to be
				   an external constant string */
    CvISXSUB_on(cv);
    CvXSUB(cv) = subaddr;

    if (name)
	process_special_blocks(name, gv, cv);

    return cv;
}

#ifdef PERL_MAD
OP *
#else
void
#endif
Perl_newFORM(pTHX_ I32 floor, OP *o, OP *block)
{
    dVAR;
    register CV *cv;
#ifdef PERL_MAD
    OP* pegop = newOP(OP_NULL, 0);
#endif

    GV * const gv = o
	? gv_fetchsv(cSVOPo->op_sv, GV_ADD, SVt_PVFM)
	: gv_fetchpvs("STDOUT", GV_ADD|GV_NOTQUAL, SVt_PVFM);

    GvMULTI_on(gv);
    if ((cv = GvFORM(gv))) {
	if (ckWARN(WARN_REDEFINE)) {
	    const line_t oldline = CopLINE(PL_curcop);
	    if (PL_parser && PL_parser->copline != NOLINE)
		CopLINE_set(PL_curcop, PL_parser->copline);
	    if (o) {
		Perl_warner(aTHX_ packWARN(WARN_REDEFINE),
			    "Format %"SVf" redefined", SVfARG(cSVOPo->op_sv));
	    } else {
		Perl_warner(aTHX_ packWARN(WARN_REDEFINE),
			    "Format STDOUT redefined");
	    }
	    CopLINE_set(PL_curcop, oldline);
	}
	SvREFCNT_dec(cv);
    }
    cv = PL_compcv;
    GvFORM(gv) = cv;
    CvGV_set(cv, gv);
    CvFILE_set_from_cop(cv, PL_curcop);


    pad_tidy(padtidy_FORMAT);
    CvROOT(cv) = newUNOP(OP_LEAVEWRITE, 0, scalarseq(block));
    CvROOT(cv)->op_private |= OPpREFCOUNTED;
    OpREFCNT_set(CvROOT(cv), 1);
#ifdef PERL_MAD
    op_getmad(o,pegop,'n');
    op_getmad_weak(block, pegop, 'b');
#else
    op_free(o);
#endif
    if (PL_parser)
	PL_parser->copline = NOLINE;
    LEAVE_SCOPE(floor);
#ifdef PERL_MAD
    return pegop;
#endif
}

OP *
Perl_newANONLIST(pTHX_ OP *o)
{
    return convert(OP_ANONLIST, OPf_SPECIAL, o);
}

OP *
Perl_newANONHASH(pTHX_ OP *o)
{
    return convert(OP_ANONHASH, OPf_SPECIAL, o);
}

OP *
Perl_newANONSUB(pTHX_ I32 floor, OP *proto, OP *block)
{
    return newANONATTRSUB(floor, proto, NULL, block);
}

OP *
Perl_newANONATTRSUB(pTHX_ I32 floor, OP *proto, OP *attrs, OP *block)
{
    return newUNOP(OP_REFGEN, 0,
	newSVOP(OP_ANONCODE, 0,
		MUTABLE_SV(newATTRSUB(floor, 0, proto, attrs, block))));
}

OP *
Perl_oopsAV(pTHX_ OP *o)
{
    dVAR;

    PERL_ARGS_ASSERT_OOPSAV;

    switch (o->op_type) {
    case OP_PADSV:
	o->op_type = OP_PADAV;
	return ref(o, OP_RV2AV);

    case OP_RV2SV:
	o->op_type = OP_RV2AV;
	ref(o, OP_RV2AV);
	break;

    default:
	Perl_ck_warner_d(aTHX_ packWARN(WARN_INTERNAL), "oops: oopsAV");
	break;
    }
    return o;
}

OP *
Perl_oopsHV(pTHX_ OP *o)
{
    dVAR;

    PERL_ARGS_ASSERT_OOPSHV;

    switch (o->op_type) {
    case OP_PADSV:
    case OP_PADAV:
	o->op_type = OP_PADHV;
	return ref(o, OP_RV2HV);

    case OP_RV2SV:
    case OP_RV2AV:
	o->op_type = OP_RV2HV;
	ref(o, OP_RV2HV);
	break;

    default:
	Perl_ck_warner_d(aTHX_ packWARN(WARN_INTERNAL), "oops: oopsHV");
	break;
    }
    return o;
}

OP *
Perl_newAVREF(pTHX_ OP *o)
{
    dVAR;

    PERL_ARGS_ASSERT_NEWAVREF;

    if (o->op_type == OP_PADANY) {
	o->op_type = OP_PADAV;
	return o;
    }
    else if ((o->op_type == OP_RV2AV || o->op_type == OP_PADAV)) {
	Perl_ck_warner_d(aTHX_ packWARN(WARN_DEPRECATED),
		       "Using an array as a reference is deprecated");
    }
    return newUNOP(OP_RV2AV, 0, scalar(o));
}

OP *
Perl_newGVREF(pTHX_ I32 type, OP *o)
{
    if (type == OP_MAPSTART || type == OP_GREPSTART || type == OP_SORT)
	return newUNOP(OP_NULL, 0, o);
    return ref(newUNOP(OP_RV2GV, OPf_REF, o), type);
}

OP *
Perl_newHVREF(pTHX_ OP *o)
{
    dVAR;

    PERL_ARGS_ASSERT_NEWHVREF;

    if (o->op_type == OP_PADANY) {
	o->op_type = OP_PADHV;
	return o;
    }
    else if ((o->op_type == OP_RV2HV || o->op_type == OP_PADHV)) {
	Perl_ck_warner_d(aTHX_ packWARN(WARN_DEPRECATED),
		       "Using a hash as a reference is deprecated");
    }
    return newUNOP(OP_RV2HV, 0, scalar(o));
}

OP *
Perl_newCVREF(pTHX_ I32 flags, OP *o)
{
    return newUNOP(OP_RV2CV, flags, scalar(o));
}

OP *
Perl_newSVREF(pTHX_ OP *o)
{
    dVAR;

    PERL_ARGS_ASSERT_NEWSVREF;

    if (o->op_type == OP_PADANY) {
	o->op_type = OP_PADSV;
	return o;
    }
    return newUNOP(OP_RV2SV, 0, scalar(o));
}

/* Check routines. See the comments at the top of this file for details
 * on when these are called */

OP *
Perl_ck_anoncode(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_CK_ANONCODE;

    cSVOPo->op_targ = pad_add_anon(cSVOPo->op_sv, o->op_type);
    if (!PL_madskills)
	cSVOPo->op_sv = NULL;
    return o;
}

OP *
Perl_ck_negate(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_CK_NEGATE;

    if (cUNOPo->op_first->op_type == OP_CONST)
	cUNOPo->op_first->op_private = ~OPpCONST_STRICT;

    return o;
}

OP *
Perl_ck_bitop(pTHX_ OP *o)
{
    dVAR;

    PERL_ARGS_ASSERT_CK_BITOP;

#define OP_IS_NUMCOMPARE(op) \
	((op) == OP_LT   || (op) == OP_I_LT || \
	 (op) == OP_GT   || (op) == OP_I_GT || \
	 (op) == OP_LE   || (op) == OP_I_LE || \
	 (op) == OP_GE   || (op) == OP_I_GE || \
	 (op) == OP_EQ   || (op) == OP_I_EQ || \
	 (op) == OP_NE   || (op) == OP_I_NE || \
	 (op) == OP_NCMP || (op) == OP_I_NCMP)

    o->op_private = (U8)(PL_hints & HINT_INTEGER);
    if (!(o->op_flags & OPf_STACKED) /* Not an assignment */
	    && (o->op_type == OP_BIT_OR
	     || o->op_type == OP_BIT_AND
	     || o->op_type == OP_BIT_XOR))
    {
	const OP * const left = cBINOPo->op_first;
	const OP * const right = left->op_sibling;
	if ((OP_IS_NUMCOMPARE(left->op_type) &&
		(left->op_flags & OPf_PARENS) == 0) ||
	    (OP_IS_NUMCOMPARE(right->op_type) &&
		(right->op_flags & OPf_PARENS) == 0))
	    Perl_ck_warner(aTHX_ packWARN(WARN_PRECEDENCE),
			   "Possible precedence problem on bitwise %c operator",
			   o->op_type == OP_BIT_OR ? '|'
			   : o->op_type == OP_BIT_AND ? '&' : '^'
			   );
    }
    return o;
}

OP *
Perl_ck_concat(pTHX_ OP *o)
{
    /* const OP * const kid = cUNOPo->op_first; */

    PERL_ARGS_ASSERT_CK_CONCAT;
    PERL_UNUSED_CONTEXT;

    /* assert(!(kid->op_private & OPpTARGET_MY)); */
    /* FIXME move to compile.c */
    /* if (kid->op_type == OP_CONCAT && !(kid->op_private & OPpTARGET_MY) && */
    /* 	    !(kUNOP->op_first->op_flags & OPf_MOD)) */
    /*     o->op_flags |= OPf_STACKED; */
    return o;
}

OP *
Perl_ck_spair(pTHX_ OP *o)
{
    dVAR;

    PERL_ARGS_ASSERT_CK_SPAIR;

    if (o->op_flags & OPf_KIDS) {
    	OP* newop;
    	OP* kid;
    	const OPCODE type = o->op_type;
    	o = modkids(ck_fun(o), type);
    	kid = cUNOPo->op_first;
    	newop = kUNOP->op_first;
    	if (newop) {
    	    const OPCODE type = newop->op_type;
    	    if (newop->op_sibling || !(PL_opargs[type] & OA_RETSCALAR) ||
    		    type == OP_PADAV || type == OP_PADHV ||
    		    type == OP_RV2AV || type == OP_RV2HV)
    		return o;
    	}
	op_null(kid);
    }
    ++o->op_type;
    return ck_fun(o);
}

OP *
Perl_ck_delete(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_CK_DELETE;

    o = ck_fun(o);
    o->op_private = 0;
    if (o->op_flags & OPf_KIDS) {
	OP * const kid = cUNOPo->op_first;
	switch (kid->op_type) {
	case OP_ASLICE:
	    o->op_flags |= OPf_SPECIAL;
	    /* FALL THROUGH */
	case OP_HSLICE:
	    o->op_private |= OPpSLICE;
	    break;
	case OP_AELEM:
	    o->op_flags |= OPf_SPECIAL;
	    /* FALL THROUGH */
	case OP_HELEM:
	    break;
	default:
	    Perl_croak(aTHX_ "%s argument is not a HASH or ARRAY element or slice",
		  OP_DESC(o));
	}
	if (kid->op_private & OPpLVAL_INTRO)
	    o->op_private |= OPpLVAL_INTRO;
	op_null(kid);
    }
    return o;
}

OP *
Perl_ck_die(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_CK_DIE;

#ifdef VMS
    if (VMSISH_HUSHED) o->op_private |= OPpHUSH_VMSISH;
#endif
    return ck_fun(o);
}

OP *
Perl_ck_eof(pTHX_ OP *o)
{
    dVAR;

    PERL_ARGS_ASSERT_CK_EOF;

    if (o->op_flags & OPf_KIDS) {
	if (cLISTOPo->op_first->op_type == OP_STUB) {
	    OP * const newop
		= newUNOP(o->op_type, OPf_SPECIAL, newGVOP(OP_GV, 0, PL_argvgv));
#ifdef PERL_MAD
	    op_getmad(o,newop,'O');
#else
	    op_free(o);
#endif
	    o = newop;
	}
	return ck_fun(o);
    }
    return o;
}

OP *
Perl_ck_eval(pTHX_ OP *o)
{
    dVAR;

    PERL_ARGS_ASSERT_CK_EVAL;

    PL_hints |= HINT_BLOCK_SCOPE;
    if (o->op_flags & OPf_KIDS) {
	SVOP * const kid = (SVOP*)cUNOPo->op_first;

	if (!kid) {
	    o->op_flags &= ~OPf_KIDS;
	    op_null(o);
	}
	else if (kid->op_type == OP_LINESEQ || kid->op_type == OP_STUB) {
#ifdef PERL_MAD
            OP* const oldo = o;
#endif
	    LOGOP* logop;

	    cUNOPo->op_first = 0;
#ifndef PERL_MAD
            op_free(o);
#endif

	    NewOp(1101, logop, 1, LOGOP);
	    o = (OP*)logop;
	    o->op_type = OP_ENTERTRY;
	    o->op_private = 0;
	    o->op_flags |= OPf_KIDS;

	    cLOGOPo->op_first = (OP*)kid;

	    cLOGOPo->op_other = o;
	    op_getmad(oldo,o,'O');
	    return o;
	}
	else {
	    scalar((OP*)kid);
	    PL_cv_has_eval = 1;
	}
    }
    else {
#ifdef PERL_MAD
	OP* const oldo = o;
#else
	op_free(o);
#endif
	o = newUNOP(OP_ENTEREVAL, 0, newDEFSVOP());
	op_getmad(oldo,o,'O');
    }
    o->op_targ = (PADOFFSET)PL_hints;
    if ((PL_hints & HINT_LOCALIZE_HH) != 0 && GvHV(PL_hintgv)) {
	/* Store a copy of %^H that pp_entereval can pick up. */
	OP *hhop = newSVOP(OP_HINTSEVAL, 0,
			   MUTABLE_SV(Perl_hv_copy_hints_hv(aTHX_ GvHV(PL_hintgv))));
	cUNOPo->op_first->op_sibling = hhop;
	o->op_private |= OPpEVAL_HAS_HH;
    }
    return o;
}

OP *
Perl_ck_exit(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_CK_EXIT;

#ifdef VMS
    HV * const table = GvHV(PL_hintgv);
    if (table) {
       SV * const * const svp = hv_fetchs(table, "vmsish_exit", FALSE);
       if (svp && *svp && SvTRUE(*svp))
           o->op_private |= OPpEXIT_VMSISH;
    }
    if (VMSISH_HUSHED) o->op_private |= OPpHUSH_VMSISH;
#endif
    return ck_fun(o);
}

OP *
Perl_ck_exec(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_CK_EXEC;

    if (o->op_flags & OPf_STACKED) {
        OP *kid;
	o = ck_fun(o);
	kid = cUNOPo->op_first;
	if (kid->op_type == OP_RV2GV)
	    op_null(kid);
    }
    else
	o = listkids(o);
    return o;
}

OP *
Perl_ck_exists(pTHX_ OP *o)
{
    dVAR;

    PERL_ARGS_ASSERT_CK_EXISTS;

    o = ck_fun(o);
    if (o->op_flags & OPf_KIDS) {
	OP * const kid = cUNOPo->op_first;
	if (kid->op_type == OP_ENTERSUB) {
	    (void) ref(kid, o->op_type);
	    if (kid->op_type != OP_RV2CV
			&& !(PL_parser && PL_parser->error_count))
		Perl_croak(aTHX_ "%s argument is not a subroutine name",
			    OP_DESC(o));
	    o->op_private |= OPpEXISTS_SUB;
	}
	else if (kid->op_type == OP_AELEM)
	    o->op_flags |= OPf_SPECIAL;
	else if (kid->op_type != OP_HELEM)
	    Perl_croak(aTHX_ "%s argument is not a HASH or ARRAY element or a subroutine",
		        OP_DESC(o));
	op_null(kid);
    }
    return o;
}

OP *
Perl_ck_rvconst(pTHX_ register OP *o)
{
    dVAR;
    SVOP * const kid = (SVOP*)cUNOPo->op_first;

    PERL_ARGS_ASSERT_CK_RVCONST;

    o->op_private |= (PL_hints & HINT_STRICT_REFS);
    if (o->op_type == OP_RV2CV)
	o->op_private &= ~1;

    if (kid->op_type == OP_CONST) {
	int iscv;
	GV *gv;
	SV * const kidsv = kid->op_sv;

	/* Is it a constant from cv_const_sv()? */
	if (SvROK(kidsv) && SvREADONLY(kidsv)) {
	    SV * const rsv = SvRV(kidsv);
	    const svtype type = SvTYPE(rsv);
            const char *badtype = NULL;

	    switch (o->op_type) {
	    case OP_RV2SV:
		if (type > SVt_PVMG)
		    badtype = "a SCALAR";
		break;
	    case OP_RV2AV:
		if (type != SVt_PVAV)
		    badtype = "an ARRAY";
		break;
	    case OP_RV2HV:
		if (type != SVt_PVHV)
		    badtype = "a HASH";
		break;
	    case OP_RV2CV:
		if (type != SVt_PVCV)
		    badtype = "a CODE";
		break;
	    }
	    if (badtype)
		Perl_croak(aTHX_ "Constant is not %s reference", badtype);
	    return o;
	}
	if ((o->op_private & HINT_STRICT_REFS) && (kid->op_private & OPpCONST_BARE)) {
	    const char *badthing;
	    switch (o->op_type) {
	    case OP_RV2SV:
		badthing = "a SCALAR";
		break;
	    case OP_RV2AV:
		badthing = "an ARRAY";
		break;
	    case OP_RV2HV:
		badthing = "a HASH";
		break;
	    default:
		badthing = NULL;
		break;
	    }
	    if (badthing)
		Perl_croak(aTHX_
			   "Can't use bareword (\"%"SVf"\") as %s ref while \"strict refs\" in use",
			   SVfARG(kidsv), badthing);
	}
	/*
	 * This is a little tricky.  We only want to add the symbol if we
	 * didn't add it in the lexer.  Otherwise we get duplicate strict
	 * warnings.  But if we didn't add it in the lexer, we must at
	 * least pretend like we wanted to add it even if it existed before,
	 * or we get possible typo warnings.  OPpCONST_ENTERED says
	 * whether the lexer already added THIS instance of this symbol.
	 */
	iscv = (o->op_type == OP_RV2CV) * 2;
	do {
	    gv = gv_fetchsv(kidsv,
		iscv | !(kid->op_private & OPpCONST_ENTERED),
		iscv
		    ? SVt_PVCV
		    : o->op_type == OP_RV2SV
			? SVt_PV
			: o->op_type == OP_RV2AV
			    ? SVt_PVAV
			    : o->op_type == OP_RV2HV
				? SVt_PVHV
				: SVt_PVGV);
	} while (!gv && !(kid->op_private & OPpCONST_ENTERED) && !iscv++);
	if (gv) {
	    kid->op_type = OP_GV;
	    SvREFCNT_dec(kid->op_sv);
#ifdef USE_ITHREADS
	    /* XXX hack: dependence on sizeof(PADOP) <= sizeof(SVOP) */
	    kPADOP->op_padix = pad_alloc(OP_GV, SVs_PADTMP);
	    SvREFCNT_dec(PAD_SVl(kPADOP->op_padix));
	    GvIN_PAD_on(gv);
	    PAD_SETSV(kPADOP->op_padix, MUTABLE_SV(SvREFCNT_inc_simple_NN(gv)));
#else
	    kid->op_sv = SvREFCNT_inc_simple_NN(gv);
#endif
	    kid->op_private = 0;
	}
    }
    return o;
}

OP *
Perl_ck_ftst(pTHX_ OP *o)
{
    dVAR;
    const I32 type = o->op_type;

    PERL_ARGS_ASSERT_CK_FTST;

    if (o->op_flags & OPf_REF) {
	NOOP;
    }
    else if (o->op_flags & OPf_KIDS && cUNOPo->op_first->op_type != OP_STUB) {
	SVOP * const kid = (SVOP*)cUNOPo->op_first;
	const OPCODE kidtype = kid->op_type;

	if (kidtype == OP_CONST && (kid->op_private & OPpCONST_BARE)) {
	    OP * const newop = newGVOP(type, OPf_REF,
		gv_fetchsv(kid->op_sv, GV_ADD, SVt_PVIO));
#ifdef PERL_MAD
	    op_getmad(o,newop,'O');
#else
	    op_free(o);
#endif
	    return newop;
	}
	if ((PL_hints & HINT_FILETEST_ACCESS) && OP_IS_FILETEST_ACCESS(o->op_type))
	    o->op_private |= OPpFT_ACCESS;
	if (PL_check[kidtype] == MEMBER_TO_FPTR(Perl_ck_ftst)
	    && kidtype != OP_STAT && kidtype != OP_LSTAT) {
	    o->op_private |= OPpFT_STACKED;
	    kid->op_private |= OPpFT_STACKING;
	}
    }
    else {
#ifdef PERL_MAD
	OP* const oldo = o;
#else
	op_free(o);
#endif
	if (type == OP_FTTTY)
	    o = newGVOP(type, OPf_REF, PL_stdingv);
	else
	    o = newUNOP(type, 0, newDEFSVOP());
	op_getmad(oldo,o,'O');
    }
    return o;
}

OP *
Perl_ck_fun(pTHX_ OP *o)
{
    dVAR;
    const int type = o->op_type;
    register I32 oa = PL_opargs[type] >> OASHIFT;

    PERL_ARGS_ASSERT_CK_FUN;

    if (o->op_flags & OPf_STACKED) {
	if ((oa & OA_OPTIONAL) && (oa >> 4) && !((oa >> 4) & OA_OPTIONAL))
	    oa &= ~OA_OPTIONAL;
	else
	    return no_fh_allowed(o);
    }

    if (o->op_flags & OPf_KIDS) {
        OP **tokid = &cLISTOPo->op_first;
        register OP *kid = cLISTOPo->op_first;
        OP *sibl;
        I32 numargs = 0;

	if (kid->op_type == OP_PUSHMARK ||
	    (kid->op_type == OP_NULL && kid->op_targ == OP_PUSHMARK))
	{
	    tokid = &kid->op_sibling;
	    kid = kid->op_sibling;
	}
	if (!kid && PL_opargs[type] & OA_DEFGV)
	    *tokid = kid = newDEFSVOP();

	while (oa && kid) {
	    numargs++;
	    sibl = kid->op_sibling;
#ifdef PERL_MAD
	    if (!sibl && kid->op_type == OP_STUB) {
		numargs--;
		break;
	    }
#endif
	    switch (oa & 7) {
	    case OA_SCALAR:
		/* list seen where single (scalar) arg expected? */
		if (numargs == 1 && !(oa >> 4)
		    && kid->op_type == OP_LIST && type != OP_SCALAR)
		{
		    return too_many_arguments(o,PL_op_desc[type]);
		}
		scalar(kid);
		break;
	    case OA_LIST:
		if (oa < 16) {
		    kid = 0;
		    continue;
		}
		else
		    list(kid);
		break;
	    case OA_AVREF:
		if ((type == OP_PUSH || type == OP_UNSHIFT)
		    && !kid->op_sibling)
		    Perl_ck_warner(aTHX_ packWARN(WARN_SYNTAX),
				   "Useless use of %s with no values",
				   PL_op_desc[type]);

		if (kid->op_type == OP_CONST &&
		    (kid->op_private & OPpCONST_BARE))
		{
		    OP * const newop = newAVREF(newGVOP(OP_GV, 0,
			gv_fetchsv(((SVOP*)kid)->op_sv, GV_ADD, SVt_PVAV) ));
		    Perl_ck_warner_d(aTHX_ packWARN(WARN_DEPRECATED),
				   "Array @%"SVf" missing the @ in argument %"IVdf" of %s()",
				   SVfARG(((SVOP*)kid)->op_sv), (IV)numargs, PL_op_desc[type]);
#ifdef PERL_MAD
		    op_getmad(kid,newop,'K');
#else
		    op_free(kid);
#endif
		    kid = newop;
		    kid->op_sibling = sibl;
		    *tokid = kid;
		}
		else if (kid->op_type != OP_RV2AV && kid->op_type != OP_PADAV)
		    bad_type(numargs, "array", PL_op_desc[type], kid);
		mod(kid, type);
		break;
	    case OA_HVREF:
		if (kid->op_type == OP_CONST &&
		    (kid->op_private & OPpCONST_BARE))
		{
		    OP * const newop = newHVREF(newGVOP(OP_GV, 0,
			gv_fetchsv(((SVOP*)kid)->op_sv, GV_ADD, SVt_PVHV) ));
		    Perl_ck_warner_d(aTHX_ packWARN(WARN_DEPRECATED),
				   "Hash %%%"SVf" missing the %% in argument %"IVdf" of %s()",
				   SVfARG(((SVOP*)kid)->op_sv), (IV)numargs, PL_op_desc[type]);
#ifdef PERL_MAD
		    op_getmad(kid,newop,'K');
#else
		    op_free(kid);
#endif
		    kid = newop;
		    kid->op_sibling = sibl;
		    *tokid = kid;
		}
		else if (kid->op_type != OP_RV2HV && kid->op_type != OP_PADHV)
		    bad_type(numargs, "hash", PL_op_desc[type], kid);
		mod(kid, type);
		break;
	    case OA_CVREF:
		{
		    OP * const newop = newUNOP(OP_NULL, 0, kid);
		    kid->op_sibling = 0;
		    kid = newop;
		    kid->op_sibling = sibl;
		    *tokid = kid;
		}
		break;
	    case OA_FILEREF:
		if (kid->op_type != OP_GV && kid->op_type != OP_RV2GV) {
		    if (kid->op_type == OP_CONST &&
			(kid->op_private & OPpCONST_BARE))
		    {
			OP * const newop = newGVOP(OP_GV, 0,
			    gv_fetchsv(((SVOP*)kid)->op_sv, GV_ADD, SVt_PVIO));
			if (!(o->op_private & 1) && /* if not unop */
			    kid == cLISTOPo->op_last)
			    cLISTOPo->op_last = newop;
#ifdef PERL_MAD
			op_getmad(kid,newop,'K');
#else
			op_free(kid);
#endif
			kid = newop;
		    }
		    else if (kid->op_type == OP_READLINE) {
			/* neophyte patrol: open(<FH>), close(<FH>) etc. */
			bad_type(numargs, "HANDLE", OP_DESC(o), kid);
		    }
		    else {
			I32 flags = OPf_SPECIAL;
			I32 priv = 0;
			PADOFFSET targ = 0;

			/* is this op a FH constructor? */
			if (is_handle_constructor(o,numargs)) {
                            const char *name = NULL;
			    STRLEN len = 0;

			    flags = 0;
			    /* Set a flag to tell rv2gv to vivify
			     * need to "prove" flag does not mean something
			     * else already - NI-S 1999/05/07
			     */
			    priv = OPpDEREF;
			    if (kid->op_type == OP_PADSV) {
				SV *const namesv
				    = PAD_COMPNAME_SV(kid->op_targ);
				name = SvPV_const(namesv, len);
			    }
			    else if (kid->op_type == OP_RV2SV
				     && kUNOP->op_first->op_type == OP_GV)
			    {
				GV * const gv = cGVOPx_gv(kUNOP->op_first);
				name = GvNAME(gv);
				len = GvNAMELEN(gv);
			    }
			    else if (kid->op_type == OP_AELEM
				     || kid->op_type == OP_HELEM)
			    {
				 OP *firstop;
				 OP *op = ((BINOP*)kid)->op_first;
				 name = NULL;
				 if (op) {
				      SV *tmpstr = NULL;
				      const char * const a =
					   kid->op_type == OP_AELEM ?
					   "[]" : "{}";
				      if (((op->op_type == OP_RV2AV) ||
					   (op->op_type == OP_RV2HV)) &&
					  (firstop = ((UNOP*)op)->op_first) &&
					  (firstop->op_type == OP_GV)) {
					   /* packagevar $a[] or $h{} */
					   GV * const gv = cGVOPx_gv(firstop);
					   if (gv)
						tmpstr =
						     Perl_newSVpvf(aTHX_
								   "%s%c...%c",
								   GvNAME(gv),
								   a[0], a[1]);
				      }
				      else if (op->op_type == OP_PADAV
					       || op->op_type == OP_PADHV) {
					   /* lexicalvar $a[] or $h{} */
					   const char * const padname =
						PAD_COMPNAME_PV(op->op_targ);
					   if (padname)
						tmpstr =
						     Perl_newSVpvf(aTHX_
								   "%s%c...%c",
								   padname + 1,
								   a[0], a[1]);
				      }
				      if (tmpstr) {
					   name = SvPV_const(tmpstr, len);
					   sv_2mortal(tmpstr);
				      }
				 }
				 if (!name) {
				      name = "__ANONIO__";
				      len = 10;
				 }
				 mod(kid, type);
			    }
			    if (name) {
				SV *namesv;
				targ = pad_alloc(OP_RV2GV, SVs_PADTMP);
				namesv = PAD_SVl(targ);
				SvUPGRADE(namesv, SVt_PV);
				if (*name != '$')
				    sv_setpvs(namesv, "$");
				sv_catpvn(namesv, name, len);
			    }
			}
			kid->op_sibling = 0;
			kid = newUNOP(OP_RV2GV, flags, scalar(kid));
			kid->op_targ = targ;
			kid->op_private |= priv;
		    }
		    kid->op_sibling = sibl;
		    *tokid = kid;
		}
		scalar(kid);
		break;
	    case OA_SCALARREF:
		mod(scalar(kid), type);
		break;
	    }
	    oa >>= 4;
	    tokid = &kid->op_sibling;
	    kid = kid->op_sibling;
	}
#ifdef PERL_MAD
	if (kid && kid->op_type != OP_STUB)
	    return too_many_arguments(o,OP_DESC(o));
	o->op_private |= numargs;
#else
	/* FIXME - should the numargs move as for the PERL_MAD case?  */
	o->op_private |= numargs;
	if (kid)
	    return too_many_arguments(o,OP_DESC(o));
#endif
	listkids(o);
    }
    else if (PL_opargs[type] & OA_DEFGV) {
#ifdef PERL_MAD
	OP *newop = newUNOP(type, 0, newDEFSVOP());
	op_getmad(o,newop,'O');
	return newop;
#else
	/* Ordering of these two is important to keep f_map.t passing.  */
	op_free(o);
	if (PL_opargs[type] & OA_LISTOP)
	    return newLISTOP(type, 0, newDEFSVOP(), NULL);
	else
	    return newUNOP(type, 0, newDEFSVOP());
#endif
    }

    if (oa) {
	while (oa & OA_OPTIONAL)
	    oa >>= 4;
	if (oa && oa != OA_LIST)
	    return too_few_arguments(o,OP_DESC(o));
    }
    return o;
}

OP *
Perl_ck_glob(pTHX_ OP *o)
{
    dVAR;
    GV *gv;

    PERL_ARGS_ASSERT_CK_GLOB;

    o = ck_fun(o);
    if (!cLISTOPo->op_first)
	append_elem(OP_GLOB, o, newDEFSVOP());

    if (!((gv = gv_fetchpvs("glob", GV_NOTQUAL, SVt_PVCV))
	  && GvCVu(gv) && GvIMPORTED_CV(gv)))
    {
	gv = gv_fetchpvs("CORE::GLOBAL::glob", 0, SVt_PVCV);
    }

#if !defined(PERL_EXTERNAL_GLOB)
    /* XXX this can be tightened up and made more failsafe. */
    if (!(gv && GvCVu(gv) && GvIMPORTED_CV(gv))) {
	GV *glob_gv;
	ENTER;
	Perl_load_module(aTHX_ PERL_LOADMOD_NOIMPORT,
		newSVpvs("File::Glob"), NULL, NULL, NULL);
	if((glob_gv = gv_fetchpvs("File::Glob::csh_glob", 0, SVt_PVCV))) {
	    gv = gv_fetchpvs("CORE::GLOBAL::glob", 0, SVt_PVCV);
	    GvCV(gv) = GvCV(glob_gv);
	    SvREFCNT_inc_void(MUTABLE_SV(GvCV(gv)));
	    GvIMPORTED_CV_on(gv);
	}
	LEAVE;
    }
#endif /* PERL_EXTERNAL_GLOB */

    if (gv && GvCVu(gv) && GvIMPORTED_CV(gv)) {
	append_elem(OP_GLOB, o,
		    newSVOP(OP_CONST, 0, newSViv(PL_glob_index++)));
	o->op_type = OP_LIST;
	o = convert(OP_ENTERSUB, OPf_STACKED,
		    append_elem(OP_LIST, o,
				scalar(newUNOP(OP_RV2CV, 0,
					       newGVOP(OP_GV, 0, gv)))));
	o = newUNOP(OP_NULL, 0, ck_subr(o));
	o->op_targ = OP_GLOB;		/* hint at what it used to be */
	return o;
    }
    gv = newGVgen("main");
    gv_IOadd(gv);
    append_elem(OP_GLOB, o, newGVOP(OP_GV, 0, gv));
    scalarkids(o);
    return o;
}

OP *
Perl_ck_grep(pTHX_ OP *o)
{
    dVAR;
    OP *kid;
    const OPCODE type = o->op_type == OP_GREPSTART ? OP_GREPWHILE : OP_MAPWHILE;
    PADOFFSET offset;

    PERL_ARGS_ASSERT_CK_GREP;

    /* don't allocate gwop here, as we may leak it if PL_parser->error_count > 0 */

    if (o->op_flags & OPf_STACKED) {
	o = ck_sort(o);
	kid = cUNOPx(cLISTOPo->op_first)->op_first;
	if (kid->op_type != OP_SCOPE && kid->op_type != OP_LEAVE)
	    return no_fh_allowed(o);
	o->op_flags &= ~OPf_STACKED;
    }
    kid = cLISTOPo->op_first;
    if (type == OP_MAPWHILE)
	list(kid);
    else
	scalar(kid);
    o = ck_fun(o);
    if (PL_parser && PL_parser->error_count)
	return o;
    kid = cLISTOPo->op_first;
    if (kid->op_type != OP_NULL)
	Perl_croak(aTHX_ "panic: ck_grep");
    kid = kUNOP->op_first;

    offset = Perl_pad_findmy(aTHX_ STR_WITH_LEN("$_"), 0);
    if (offset == NOT_IN_PAD || PAD_COMPNAME_FLAGS_isOUR(offset)) {
	o->op_private = 0;
	o->op_targ = pad_alloc(type, SVs_PADTMP);
    }
    else {
	o->op_private = OPpGREP_LEX;
	o->op_targ = offset;
    }

    kid = cLISTOPo->op_first;
    if (!kid || !kid->op_sibling)
	return too_few_arguments(o,OP_DESC(o));
    for (kid = kid->op_sibling; kid; kid = kid->op_sibling)
	mod(kid, OP_GREPSTART);

    return o;
}

OP *
Perl_ck_index(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_CK_INDEX;

    if (o->op_flags & OPf_KIDS) {
	OP *kid = cLISTOPo->op_first;
	if (kid)
	    kid = kid->op_sibling;			/* get past "big" */
	if (kid && kid->op_type == OP_CONST)
	    fbm_compile(((SVOP*)kid)->op_sv, 0);
    }
    return ck_fun(o);
}

OP*
Perl_ck_formline(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_CK_FORMLINE;

    return ck_fun(o);
}

OP *
Perl_ck_lfun(pTHX_ OP *o)
{
    const OPCODE type = o->op_type;

    PERL_ARGS_ASSERT_CK_LFUN;

    return modkids(ck_fun(o), type);
}

OP *
Perl_ck_defined(pTHX_ OP *o)		/* 19990527 MJD */
{
    PERL_ARGS_ASSERT_CK_DEFINED;

    if ((o->op_flags & OPf_KIDS)) {
	switch (cUNOPo->op_first->op_type) {
	case OP_RV2AV:
	    /* This is needed for
	       if (defined %stash::)
	       to work.   Do not break Tk.
	       */
	    break;                      /* Globals via GV can be undef */
	case OP_PADAV:
	case OP_AASSIGN:		/* Is this a good idea? */
	    Perl_ck_warner_d(aTHX_ packWARN(WARN_DEPRECATED),
			   "defined(@array) is deprecated");
	    Perl_ck_warner_d(aTHX_ packWARN(WARN_DEPRECATED),
			   "\t(Maybe you should just omit the defined()?)\n");
	break;
	case OP_RV2HV:
	case OP_PADHV:
	    Perl_ck_warner_d(aTHX_ packWARN(WARN_DEPRECATED),
			   "defined(%%hash) is deprecated");
	    Perl_ck_warner_d(aTHX_ packWARN(WARN_DEPRECATED),
			   "\t(Maybe you should just omit the defined()?)\n");
	    break;
	default:
	    /* no warning */
	    break;
	}
    }
    return ck_rfun(o);
}

OP *
Perl_ck_readline(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_CK_READLINE;

    if (!(o->op_flags & OPf_KIDS)) {
	OP * const newop
	    = newUNOP(OP_READLINE, 0, newGVOP(OP_GV, 0, PL_argvgv));
#ifdef PERL_MAD
	op_getmad(o,newop,'O');
#else
	op_free(o);
#endif
	return newop;
    }
    return o;
}

OP *
Perl_ck_rfun(pTHX_ OP *o)
{
    const OPCODE type = o->op_type;

    PERL_ARGS_ASSERT_CK_RFUN;

    return refkids(ck_fun(o), type);
}

OP *
Perl_ck_listiob(pTHX_ OP *o)
{
    register OP *kid;

    PERL_ARGS_ASSERT_CK_LISTIOB;

    kid = cLISTOPo->op_first;
    if (kid && o->op_flags & OPf_STACKED)
	kid = kid->op_sibling;
    else if (kid && !kid->op_sibling) {		/* print HANDLE; */
	if (kid->op_type == OP_CONST && kid->op_private & OPpCONST_BARE) {
	    o->op_flags |= OPf_STACKED;	/* make it a filehandle */
	    kid = newUNOP(OP_RV2GV, OPf_REF, scalar(kid));
	    cLISTOPo->op_first = kid;
	    cLISTOPo->op_last = kid;
	    kid = kid->op_sibling;
	}
    }

    if (!kid)
	append_elem(o->op_type, o, newDEFSVOP());

    return listkids(o);
}

OP *
Perl_ck_smartmatch(pTHX_ OP *o)
{
    dVAR;
    if (0 == (o->op_flags & OPf_SPECIAL)) {
	OP *first  = cBINOPo->op_first;
	OP *second = first->op_sibling;
	
	/* Implicitly take a reference to an array or hash */
	first->op_sibling = NULL;
	first = cBINOPo->op_first = ref_array_or_hash(first);
	second = first->op_sibling = ref_array_or_hash(second);
	
	/* Implicitly take a reference to a regular expression */
	if (first->op_type == OP_MATCH) {
	    first->op_type = OP_QR;
	}
	if (second->op_type == OP_MATCH) {
	    second->op_type = OP_QR;
        }
    }
    
    return o;
}


OP *
Perl_ck_sassign(pTHX_ OP *o)
{
    dVAR;
    OP * const kid = cLISTOPo->op_first;

    PERL_ARGS_ASSERT_CK_SASSIGN;

    /* has a disposable target? */
    /* FIXME Move to compile.c */
    /* if ((PL_opargs[kid->op_type] & OA_TARGLEX) */
    /* 	&& !(kid->op_flags & OPf_STACKED) */
    /* 	/\* Cannot steal the second time! *\/ */
    /* 	&& !(kid->op_private & OPpTARGET_MY) */
    /* 	/\* Keep the full thing for madskills *\/ */
    /* 	&& !PL_madskills */
    /* 	) */
    /* { */
    /* 	OP * const kkid = kid->op_sibling; */

    /* 	/\* Can just relocate the target. *\/ */
    /* 	if (kkid && kkid->op_type == OP_PADSV */
    /* 	    && !(kkid->op_private & OPpLVAL_INTRO)) */
    /* 	{ */
    /* 	    kid->op_targ = kkid->op_targ; */
    /* 	    kkid->op_targ = 0; */
    /* 	    /\* Now we do not need PADSV and SASSIGN. *\/ */
    /* 	    kid->op_sibling = o->op_sibling;	/\* NULL *\/ */
    /* 	    cLISTOPo->op_first = NULL; */
    /* 	    op_free(o); */
    /* 	    op_free(kkid); */
    /* 	    kid->op_private |= OPpTARGET_MY;	/\* Used for context settings *\/ */
    /* 	    return kid; */
    /* 	} */
    /* } */
    if (kid->op_sibling) {
	OP *kkid = kid->op_sibling;
	if (kkid->op_type == OP_PADSV
		&& (kkid->op_private & OPpLVAL_INTRO)
		&& SvPAD_STATE(*av_fetch(PL_comppad_name, kkid->op_targ, FALSE))) {
	    const PADOFFSET target = kkid->op_targ;
	    OP *const other = newOP(OP_PADSV,
				    kkid->op_flags
				    | ((kkid->op_private & ~OPpLVAL_INTRO) << 8));

	    LOGOP *condop;

	    scalar(o);
	    scalar(other);

	    NewOp(1101, condop, 1, LOGOP);
	    condop->op_type = OP_ONCE;
	    condop->op_first = o;
	    condop->op_flags = OPf_KIDS;
	    condop->op_other_instr = NULL;
	    condop->op_private = 1;

	    o->op_sibling = other;
	    
	    CHECKOP(OP_ONCE, condop);

	    /* hijacking PADSTALE for uninitialized state variables */
	    SvPADSTALE_on(PAD_SVl(target));

	    condop->op_targ = target;
	    other->op_targ = target;

	    /* Because we change the type of the op here, we will skip the
	       assinment binop->op_last = binop->op_first->op_sibling; at the
	       end of Perl_newBINOP(). So need to do it here. */
	    cBINOPo->op_last = cBINOPo->op_first->op_sibling;

	    return (OP*)condop;
	}
    }
    return o;
}

OP *
Perl_ck_match(pTHX_ OP *o)
{
    dVAR;

    PERL_ARGS_ASSERT_CK_MATCH;

    if (o->op_type != OP_QR && PL_compcv) {
	const PADOFFSET offset = Perl_pad_findmy(aTHX_ STR_WITH_LEN("$_"), 0);
	if (offset != NOT_IN_PAD && !(PAD_COMPNAME_FLAGS_isOUR(offset))) {
	    o->op_targ = offset;
	    o->op_private |= OPpTARGET_MY;
	}
    }
    if (o->op_type == OP_MATCH || o->op_type == OP_QR)
	o->op_private |= OPpRUNTIME;
    return o;
}

OP *
Perl_ck_method(pTHX_ OP *o)
{
    OP * const kid = cUNOPo->op_first;

    PERL_ARGS_ASSERT_CK_METHOD;

    if (kid->op_type == OP_CONST) {
	SV* sv = kSVOP->op_sv;
	const char * const method = SvPVX_const(sv);
	if (!(strchr(method, ':') || strchr(method, '\''))) {
	    OP *cmop;
	    if (!SvREADONLY(sv) || !SvFAKE(sv)) {
		sv = newSVpvn_share(method, SvCUR(sv), 0);
	    }
	    else {
		kSVOP->op_sv = NULL;
	    }
	    cmop = newSVOP(OP_METHOD_NAMED, 0, sv);
#ifdef PERL_MAD
	    op_getmad(o,cmop,'O');
#else
	    op_free(o);
#endif
	    return cmop;
	}
    }
    return o;
}

OP *
Perl_ck_null(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_CK_NULL;
    PERL_UNUSED_CONTEXT;
    return o;
}

OP *
Perl_ck_open(pTHX_ OP *o)
{
    dVAR;
    HV * const table = GvHV(PL_hintgv);

    PERL_ARGS_ASSERT_CK_OPEN;

    if (table) {
	SV **svp = hv_fetchs(table, "open_IN", FALSE);
	if (svp && *svp) {
	    STRLEN len = 0;
	    const char *d = SvPV_const(*svp, len);
	    const I32 mode = mode_from_discipline(d, len);
	    if (mode & O_BINARY)
		o->op_private |= OPpOPEN_IN_RAW;
	    else if (mode & O_TEXT)
		o->op_private |= OPpOPEN_IN_CRLF;
	}

	svp = hv_fetchs(table, "open_OUT", FALSE);
	if (svp && *svp) {
	    STRLEN len = 0;
	    const char *d = SvPV_const(*svp, len);
	    const I32 mode = mode_from_discipline(d, len);
	    if (mode & O_BINARY)
		o->op_private |= OPpOPEN_OUT_RAW;
	    else if (mode & O_TEXT)
		o->op_private |= OPpOPEN_OUT_CRLF;
	}
    }
    if (o->op_type == OP_BACKTICK) {
	if (!(o->op_flags & OPf_KIDS)) {
	    OP * const newop = newUNOP(OP_BACKTICK, 0, newDEFSVOP());
#ifdef PERL_MAD
	    op_getmad(o,newop,'O');
#else
	    op_free(o);
#endif
	    return newop;
	}
	return o;
    }
    {
	 /* In case of three-arg dup open remove strictness
	  * from the last arg if it is a bareword. */
	 OP * const first = cLISTOPx(o)->op_first; /* The pushmark. */
	 OP * const last  = cLISTOPx(o)->op_last;  /* The bareword. */
	 OP *oa;
	 const char *mode;

	 if ((last->op_type == OP_CONST) &&		/* The bareword. */
	     (last->op_private & OPpCONST_BARE) &&
	     (last->op_private & OPpCONST_STRICT) &&
	     (oa = first) &&		                /* The fh. */
	     (oa = oa->op_sibling) &&			/* The mode. */
	     (oa->op_type == OP_CONST) &&
	     SvPOK(((SVOP*)oa)->op_sv) &&
	     (mode = SvPVX_const(((SVOP*)oa)->op_sv)) &&
	     mode[0] == '>' && mode[1] == '&' &&	/* A dup open. */
	     (last == oa->op_sibling))			/* The bareword. */
	      last->op_private &= ~OPpCONST_STRICT;
    }
    return ck_fun(o);
}

OP *
Perl_ck_repeat(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_CK_REPEAT;

    if (cBINOPo->op_first->op_flags & OPf_PARENS) {
	o->op_private |= OPpREPEAT_DOLIST;
	cBINOPo->op_first = list(force_list(cBINOPo->op_first));
    }
    else
	scalar(o);
    return o;
}

OP *
Perl_ck_require(pTHX_ OP *o)
{
    dVAR;
    GV* gv = NULL;

    PERL_ARGS_ASSERT_CK_REQUIRE;

    if (o->op_flags & OPf_KIDS) {	/* Shall we supply missing .pm? */
	SVOP * const kid = (SVOP*)cUNOPo->op_first;

	if (kid->op_type == OP_CONST && (kid->op_private & OPpCONST_BARE)) {
	    SV * const sv = kid->op_sv;
	    U32 was_readonly = SvREADONLY(sv);
	    char *s;
	    STRLEN len;
	    const char *end;

	    if (was_readonly) {
		if (SvFAKE(sv)) {
		    sv_force_normal_flags(sv, 0);
		    assert(!SvREADONLY(sv));
		    was_readonly = 0;
		} else {
		    SvREADONLY_off(sv);
		}
	    }   

	    s = SvPVX(sv);
	    len = SvCUR(sv);
	    end = s + len;
	    for (; s < end; s++) {
		if (*s == ':' && s[1] == ':') {
		    *s = '/';
		    Move(s+2, s+1, end - s - 1, char);
		    --end;
		}
	    }
	    SvEND_set(sv, end);
	    sv_catpvs(sv, ".pm");
	    SvFLAGS(sv) |= was_readonly;
	}
    }

    if (!(o->op_flags & OPf_SPECIAL)) { /* Wasn't written as CORE::require */
	/* handle override, if any */
	gv = gv_fetchpvs("require", GV_NOTQUAL, SVt_PVCV);
	if (!(gv && GvCVu(gv) && GvIMPORTED_CV(gv))) {
	    GV * const * const gvp = (GV**)hv_fetchs(PL_globalstash, "require", FALSE);
	    gv = gvp ? *gvp : NULL;
	}
    }

    if (gv && GvCVu(gv) && GvIMPORTED_CV(gv)) {
	OP * const kid = cUNOPo->op_first;
	OP * newop;

	cUNOPo->op_first = 0;
#ifndef PERL_MAD
	op_free(o);
#endif
	newop = ck_subr(convert(OP_ENTERSUB, OPf_STACKED,
				append_elem(OP_LIST, kid,
					    scalar(newUNOP(OP_RV2CV, 0,
							   newGVOP(OP_GV, 0,
								   gv))))));
	op_getmad(o,newop,'O');
	return newop;
    }

    return scalar(ck_fun(o));
}

OP *
Perl_ck_return(pTHX_ OP *o)
{
    dVAR;
    OP *kid;

    PERL_ARGS_ASSERT_CK_RETURN;

    kid = cLISTOPo->op_first;
    if (CvLVALUE(PL_compcv)) {
	for (; kid; kid = kid->op_sibling)
	    mod(kid, OP_LEAVESUBLV);
    } else {
	for (; kid; kid = kid->op_sibling)
	    if ((kid->op_type == OP_NULL)
		&& ((kid->op_flags & (OPf_SPECIAL|OPf_KIDS)) == (OPf_SPECIAL|OPf_KIDS))) {
		/* This is a do block */
		OP *op = kUNOP->op_first;
		if (op->op_type == OP_LEAVE && op->op_flags & OPf_KIDS) {
		    op = cUNOPx(op)->op_first;
		    assert(op->op_type == OP_ENTER && !(op->op_flags & OPf_SPECIAL));
		    /* Force the use of the caller's context */
		    op->op_flags |= OPf_SPECIAL;
		}
	    }
    }

    /* return is always in unknown context */
    for (kid = cUNOPo->op_first; kid; kid = kid->op_sibling)
	S_unknown(kid);
    o->op_context_known = TRUE;

    return o;
}

OP *
Perl_ck_select(pTHX_ OP *o)
{
    dVAR;
    OP* kid;

    PERL_ARGS_ASSERT_CK_SELECT;

    if (o->op_flags & OPf_KIDS) {
	kid = cLISTOPo->op_first;
	if (kid && kid->op_sibling) {
	    o->op_type = OP_SSELECT;
	    o = ck_fun(o);
	    return fold_constants(o);
	}
    }
    o = ck_fun(o);
    kid = cLISTOPo->op_first;
    if (kid && kid->op_type == OP_RV2GV)
	kid->op_private &= ~HINT_STRICT_REFS;
    return o;
}

OP *
Perl_ck_shift(pTHX_ OP *o)
{
    dVAR;
    const I32 type = o->op_type;

    PERL_ARGS_ASSERT_CK_SHIFT;

    if (!(o->op_flags & OPf_KIDS)) {
	OP *argop;

	if (!CvUNIQUE(PL_compcv)) {
	    o->op_flags |= OPf_SPECIAL;
	    return o;
	}

	argop = newUNOP(OP_RV2AV, 0, scalar(newGVOP(OP_GV, 0, PL_argvgv)));
#ifdef PERL_MAD
	OP * const oldo = o;
	o = newUNOP(type, 0, scalar(argop));
	op_getmad(oldo,o,'O');
	return o;
#else
	op_free(o);
	return newUNOP(type, 0, scalar(argop));
#endif
    }
    return scalar(modkids(ck_fun(o), type));
}

OP *
Perl_ck_sort(pTHX_ OP *o)
{
    dVAR;
    OP *firstkid;

    PERL_ARGS_ASSERT_CK_SORT;

    if (o->op_type == OP_SORT && (PL_hints & HINT_LOCALIZE_HH) != 0) {
	HV * const hinthv = GvHV(PL_hintgv);
	if (hinthv) {
	    SV ** const svp = hv_fetchs(hinthv, "sort", FALSE);
	    if (svp) {
		const I32 sorthints = (I32)SvIV(*svp);
		if ((sorthints & HINT_SORT_QUICKSORT) != 0)
		    o->op_private |= OPpSORT_QSORT;
		if ((sorthints & HINT_SORT_STABLE) != 0)
		    o->op_private |= OPpSORT_STABLE;
	    }
	}
    }

    if (o->op_type == OP_SORT && o->op_flags & OPf_STACKED)
	simplify_sort(o);
    firstkid = cLISTOPo->op_first;
    if (o->op_flags & OPf_STACKED) {			/* may have been cleared */
	OP *kid = cUNOPx(firstkid)->op_first;		/* get past null */

	if (kid->op_type == OP_SCOPE || kid->op_type == OP_LEAVE) {
	    kid = firstkid;
	    if (o->op_type == OP_SORT) {
		/* provide scalar context for comparison function/block */
		kid = scalar(kid);
	    }
	    o->op_flags |= OPf_SPECIAL;
	}
	else if (kid->op_type == OP_RV2SV || kid->op_type == OP_PADSV)
	    op_null(firstkid);

	firstkid = firstkid->op_sibling;
    }

    /* provide list context for arguments */
    if (o->op_type == OP_SORT)
	list(firstkid);

    return o;
}

STATIC void
S_simplify_sort(pTHX_ OP *o)
{
    dVAR;
    register OP *kid = cLISTOPo->op_first;
    OP *k;
    int descending;
    GV *gv;
    const char *gvname;

    PERL_ARGS_ASSERT_SIMPLIFY_SORT;

    if (!(o->op_flags & OPf_STACKED))
	return;
    GvMULTI_on(gv_fetchpvs("a", GV_ADD|GV_NOTQUAL, SVt_PV));
    GvMULTI_on(gv_fetchpvs("b", GV_ADD|GV_NOTQUAL, SVt_PV));
    kid = kUNOP->op_first;				/* get past null */
    if (kid->op_type != OP_SCOPE)
	return;
    kid = kLISTOP->op_last;				/* get past scope */
    switch(kid->op_type) {
	case OP_NCMP:
	case OP_I_NCMP:
	case OP_SCMP:
	    break;
	default:
	    return;
    }
    k = kid;						/* remember this node*/
    if (kBINOP->op_first->op_type != OP_RV2SV)
	return;
    kid = kBINOP->op_first;				/* get past cmp */
    if (kUNOP->op_first->op_type != OP_GV)
	return;
    kid = kUNOP->op_first;				/* get past rv2sv */
    gv = kGVOP_gv;
    if (GvSTASH(gv) != PL_curstash)
	return;
    gvname = GvNAME(gv);
    if (*gvname == 'a' && gvname[1] == '\0')
	descending = 0;
    else if (*gvname == 'b' && gvname[1] == '\0')
	descending = 1;
    else
	return;

    kid = k;						/* back to cmp */
    if (kBINOP->op_last->op_type != OP_RV2SV)
	return;
    kid = kBINOP->op_last;				/* down to 2nd arg */
    if (kUNOP->op_first->op_type != OP_GV)
	return;
    kid = kUNOP->op_first;				/* get past rv2sv */
    gv = kGVOP_gv;
    if (GvSTASH(gv) != PL_curstash)
	return;
    gvname = GvNAME(gv);
    if ( descending
	 ? !(*gvname == 'a' && gvname[1] == '\0')
	 : !(*gvname == 'b' && gvname[1] == '\0'))
	return;
    o->op_flags &= ~(OPf_STACKED | OPf_SPECIAL);
    if (descending)
	o->op_private |= OPpSORT_DESCEND;
    if (k->op_type == OP_NCMP)
	o->op_private |= OPpSORT_NUMERIC;
    if (k->op_type == OP_I_NCMP)
	o->op_private |= OPpSORT_NUMERIC | OPpSORT_INTEGER;
    kid = cLISTOPo->op_first;
    cLISTOPo->op_first = kid->op_sibling; /* bypass old block */
#ifdef PERL_MAD
    op_getmad(kid,o,'S');			      /* then delete it */
#else
    op_free(kid);				      /* then delete it */
#endif
}

OP *
Perl_ck_split(pTHX_ OP *o)
{
    dVAR;
    register OP *kid;

    PERL_ARGS_ASSERT_CK_SPLIT;

    if (o->op_flags & OPf_STACKED)
	return no_fh_allowed(o);

    kid = cLISTOPo->op_first;
    if (!kid) {
	cLISTOPo->op_first = kid = newSVOP(OP_CONST, 0, newSVpvs(" "));
	cLISTOPo->op_last = kid; /* There was only one element previously */
	cLISTOPo->op_flags |= OPf_KIDS;
    }

    if (kid->op_type != OP_MATCH || kid->op_flags & OPf_STACKED) {
	OP * const sibl = kid->op_sibling;
	kid->op_sibling = 0;
	kid = pmruntime( newPMOP(OP_MATCH, OPf_SPECIAL), kid, 0);
	if (cLISTOPo->op_first == cLISTOPo->op_last)
	    cLISTOPo->op_last = kid;
	cLISTOPo->op_first = kid;
	kid->op_sibling = sibl;
    }

    kid->op_type = OP_PUSHRE;
    scalar(kid);
    if (((PMOP *)kid)->op_pmflags & PMf_GLOBAL) {
      Perl_ck_warner(aTHX_ packWARN(WARN_REGEXP),
		     "Use of /g modifier is meaningless in split");
    }

    if (!kid->op_sibling)
	append_elem(OP_SPLIT, o, newDEFSVOP());

    kid = kid->op_sibling;
    scalar(kid);

    if (!kid->op_sibling)
	append_elem(OP_SPLIT, o, newSVOP(OP_CONST, 0, newSViv(0)));
    assert(kid->op_sibling);

    kid = kid->op_sibling;
    scalar(kid);

    if (kid->op_sibling)
	return too_many_arguments(o,OP_DESC(o));

    return o;
}

OP *
Perl_ck_join(pTHX_ OP *o)
{
    const OP * const kid = cLISTOPo->op_first;

    PERL_ARGS_ASSERT_CK_JOIN;

    if (kid && kid->op_type == OP_MATCH) {
	if (ckWARN(WARN_SYNTAX)) {
            const REGEXP *re = PM_GETRE(kPMOP);
	    const char *pmstr = re ? RX_PRECOMP_const(re) : "STRING";
	    const STRLEN len = re ? RX_PRELEN(re) : 6;
	    Perl_warner(aTHX_ packWARN(WARN_SYNTAX),
			"/%.*s/ should probably be written as \"%.*s\"",
			(int)len, pmstr, (int)len, pmstr);
	}
    }
    return ck_fun(o);
}

OP *
Perl_ck_subr(pTHX_ OP *o)
{
    dVAR;
    UNOP *oroot = cUNOPo;
    OP *o2 = oroot->op_first;
    OP *prev = NULL;
    OP *cvop;
    const char *proto = NULL;
    const char *proto_end = NULL;
    CV *cv = NULL;
    GV *namegv = NULL;
    int optional = 0;
    I32 arg = 0;
    I32 contextclass = 0;
    const char *e = NULL;
    bool delete_op = 0;

    PERL_ARGS_ASSERT_CK_SUBR;

    o->op_private |= OPpENTERSUB_HASTARG;
    for (cvop = o2; cvop->op_sibling; cvop = cvop->op_sibling) ;
    if (cvop->op_type == OP_RV2CV) {
	o->op_private |= (cvop->op_private & OPpENTERSUB_AMPER);
	op_null(cvop);		/* disable rv2cv */
	if (!(o->op_private & OPpENTERSUB_AMPER)) {
	    SVOP *tmpop = (SVOP*)((UNOP*)cvop)->op_first;
	    GV *gv = NULL;
	    switch (tmpop->op_type) {
		case OP_GV: {
		    gv = cGVOPx_gv(tmpop);
		    cv = GvCVu(gv);
		    if (!cv)
			tmpop->op_private |= OPpEARLY_CV;
		} break;
		case OP_CONST: {
		    SV *sv = cSVOPx_sv(tmpop);
		    if (SvROK(sv) && SvTYPE(SvRV(sv)) == SVt_PVCV)
			cv = (CV*)SvRV(sv);
		} break;
	    }
	    if (cv && SvPOK(cv)) {
		STRLEN len;
		namegv = gv && CvANON(cv) ? gv : CvGV(cv);
		proto = SvPV(MUTABLE_SV(cv), len);
		proto_end = proto + len;
	    }
	}
    }
    else if (cvop->op_type == OP_METHOD || cvop->op_type == OP_METHOD_NAMED) {
	if (o2->op_type == OP_CONST)
	    o2->op_private &= ~OPpCONST_STRICT;
	else if (o2->op_type == OP_LIST) {
	    OP * const sib = ((UNOP*)o2)->op_first->op_sibling;
	    if (sib && sib->op_type == OP_CONST)
		sib->op_private &= ~OPpCONST_STRICT;
	}
    }
    o->op_private |= (PL_hints & HINT_STRICT_REFS);
    if (PERLDB_SUB && PL_curstash != PL_debstash)
	o->op_private |= OPpENTERSUB_DB;
    while (o2 != cvop) {
	OP* o3;
	if (PL_madskills && o2->op_type == OP_STUB) {
	    o2 = o2->op_sibling;
	    continue;
	}
	if (PL_madskills && o2->op_type == OP_NULL)
	    o3 = ((UNOP*)o2)->op_first;
	else
	    o3 = o2;
	if (proto) {
	    if (proto >= proto_end)
		return too_many_arguments(o, gv_ename(namegv));

	    switch (*proto) {
	    case ';':
		optional = 1;
		proto++;
		continue;
	    case '_':
		/* _ must be at the end */
		if (proto[1] && proto[1] != ';')
		    goto oops;
	    case '$':
		proto++;
		arg++;
		scalar(o2);
		break;
	    case '%':
	    case '@':
		list(o2);
		arg++;
		break;
	    case '&':
		proto++;
		arg++;
		if (o3->op_type != OP_REFGEN && o3->op_type != OP_UNDEF)
		    bad_type(arg,
			arg == 1 ? "block or sub {}" : "sub {}",
			gv_ename(namegv), o3);
		break;
	    case '*':
		/* '*' allows any scalar type, including bareword */
		proto++;
		arg++;
		if (o3->op_type == OP_RV2GV)
		    goto wrapref;	/* autoconvert GLOB -> GLOBref */
		else if (o3->op_type == OP_CONST)
		    o3->op_private &= ~OPpCONST_STRICT;
		else if (o3->op_type == OP_ENTERSUB) {
		    /* accidental subroutine, revert to bareword */
		    OP *gvop = ((UNOP*)o3)->op_first;
			if (gvop) {
			    for (; gvop->op_sibling; gvop = gvop->op_sibling)
				;
			    if (gvop &&
				(gvop->op_private & OPpENTERSUB_NOPAREN) &&
				(gvop = ((UNOP*)gvop)->op_first) &&
				gvop->op_type == OP_GV)
			    {
				GV * const gv = cGVOPx_gv(gvop);
				OP * const sibling = o2->op_sibling;
				SV * const n = newSVpvs("");
#ifdef PERL_MAD
				OP * const oldo2 = o2;
#else
				op_free(o2);
#endif
				gv_fullname4(n, gv, "", FALSE);
				o2 = newSVOP(OP_CONST, 0, n);
				op_getmad(oldo2,o2,'O');
				if (prev)
				    prev->op_sibling = o2;
				else
				    oroot->op_first = o2;
				o2->op_sibling = sibling;
			    }
			}
		}
		scalar(o2);
		break;
	    case '[': case ']':
		 goto oops;
		 break;
	    case '\\':
		proto++;
		arg++;
	    again:
		switch (*proto++) {
		case '[':
		     if (contextclass++ == 0) {
		          e = strchr(proto, ']');
			  if (!e || e == proto)
			       goto oops;
		     }
		     else
			  goto oops;
		     goto again;
		     break;
		case ']':
		     if (contextclass) {
		         const char *p = proto;
			 const char *const end = proto;
			 contextclass = 0;
			 while (*--p != '[') {}
			 bad_type(arg, Perl_form(aTHX_ "one of %.*s",
						 (int)(end - p), p),
				  gv_ename(namegv), o3);
		     } else
			  goto oops;
		     break;
		case '*':
		     if (o3->op_type == OP_RV2GV)
			  goto wrapref;
		     if (!contextclass)
			  bad_type(arg, "symbol", gv_ename(namegv), o3);
		     break;
		case '&':
		     if (o3->op_type == OP_ENTERSUB)
			  goto wrapref;
		     if (!contextclass)
			  bad_type(arg, "subroutine entry", gv_ename(namegv),
				   o3);
		     break;
		case '$':
		    if (o3->op_type == OP_RV2SV ||
			o3->op_type == OP_PADSV ||
			o3->op_type == OP_HELEM ||
			o3->op_type == OP_AELEM)
			 goto wrapref;
		    if (!contextclass)
			bad_type(arg, "scalar", gv_ename(namegv), o3);
		     break;
		case '@':
		    if (o3->op_type == OP_RV2AV ||
			o3->op_type == OP_PADAV)
			 goto wrapref;
		    if (!contextclass)
			bad_type(arg, "array", gv_ename(namegv), o3);
		    break;
		case '%':
		    if (o3->op_type == OP_RV2HV ||
			o3->op_type == OP_PADHV)
			 goto wrapref;
		    if (!contextclass)
			 bad_type(arg, "hash", gv_ename(namegv), o3);
		    break;
		wrapref:
		    {
			OP* const kid = o2;
			OP* const sib = kid->op_sibling;
			kid->op_sibling = 0;
			o2 = newUNOP(OP_REFGEN, 0, kid);
			o2->op_sibling = sib;
			if (prev)
			    prev->op_sibling = o2;
			else
			    oroot->op_first = o2;
		    }
		    if (contextclass && e) {
			 proto = e + 1;
			 contextclass = 0;
		    }
		    break;
		default: goto oops;
		}
		if (contextclass)
		     goto again;
		break;
	    case ' ':
		proto++;
		continue;
	    default:
	      oops:
		Perl_croak(aTHX_ "Malformed prototype for %s: %"SVf,
			   gv_ename(namegv), SVfARG(cv));
	    }
	}
	else
	    list(o2);
	mod(o2, OP_ENTERSUB);
	prev = o2;
	o2 = o2->op_sibling;
    } /* while */
    if (o2 == cvop && proto && *proto == '_') {
	/* generate an access to $_ */
	o2 = newDEFSVOP();
	if (prev) {
	    o2->op_sibling = prev->op_sibling;
	    prev->op_sibling = o2; /* instead of cvop */
	}
	else {
	    o2->op_sibling = oroot->op_first;
	    oroot->op_first = o2;
	}
    }
    if (proto && !optional && proto_end > proto &&
	(*proto != '@' && *proto != '%' && *proto != ';' && *proto != '_'))
	return too_few_arguments(o, gv_ename(namegv));
    if(delete_op) {
#ifdef PERL_MAD
	OP * const oldo = o;
#else
	op_free(o);
#endif
	o=newSVOP(OP_CONST, 0, newSViv(0));
	op_getmad(oldo,o,'O');
    }
    return o;
}

OP *
Perl_ck_svconst(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_CK_SVCONST;
    PERL_UNUSED_CONTEXT;
    SvREADONLY_on(cSVOPo->op_sv);
    return o;
}

OP *
Perl_ck_chdir(pTHX_ OP *o)
{
    if (o->op_flags & OPf_KIDS) {
	SVOP * const kid = (SVOP*)cUNOPo->op_first;

	if (kid && kid->op_type == OP_CONST &&
	    (kid->op_private & OPpCONST_BARE))
	{
	    o->op_flags |= OPf_SPECIAL;
	    kid->op_private &= ~OPpCONST_STRICT;
	}
    }
    return ck_fun(o);
}

OP *
Perl_ck_trunc(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_CK_TRUNC;

    if (o->op_flags & OPf_KIDS) {
	SVOP *kid = (SVOP*)cUNOPo->op_first;

	if (kid->op_type == OP_NULL)
	    kid = (SVOP*)kid->op_sibling;
	if (kid && kid->op_type == OP_CONST &&
	    (kid->op_private & OPpCONST_BARE))
	{
	    o->op_flags |= OPf_SPECIAL;
	    kid->op_private &= ~OPpCONST_STRICT;
	}
    }
    return ck_fun(o);
}

OP *
Perl_ck_unpack(pTHX_ OP *o)
{
    OP *kid = cLISTOPo->op_first;

    PERL_ARGS_ASSERT_CK_UNPACK;

    if (kid) {
	if (!kid->op_sibling)
	    kid->op_sibling = newDEFSVOP();
    }
    return ck_fun(o);
}

OP *
Perl_ck_substr(pTHX_ OP *o)
{
    PERL_ARGS_ASSERT_CK_SUBSTR;

    o = ck_fun(o);
    if ((o->op_flags & OPf_KIDS) && (o->op_private == 4)) {
	OP *kid = cLISTOPo->op_first;

	if (kid)
	    kid->op_flags |= OPf_MOD;

    }
    return o;
}

OP *
Perl_ck_each(pTHX_ OP *o)
{
    dVAR;
    OP *kid = o->op_flags & OPf_KIDS ? cLISTOPo->op_first : NULL;

    PERL_ARGS_ASSERT_CK_EACH;

    if (kid) {
	if (kid->op_type == OP_PADAV || kid->op_type == OP_RV2AV) {
	    const unsigned new_type = o->op_type == OP_EACH ? OP_AEACH
		: o->op_type == OP_KEYS ? OP_AKEYS : OP_AVALUES;
	    o->op_type = new_type;
	}
	else if (!(kid->op_type == OP_PADHV || kid->op_type == OP_RV2HV
		    || (kid->op_type == OP_CONST && kid->op_private & OPpCONST_BARE)
		  )) {
	    bad_type(1, "hash or array", PL_op_desc[o->op_type], kid);
	    return o;
	}
    }
    return ck_fun(o);
}

const char*
Perl_custom_op_name(pTHX_ const OP* o)
{
    dVAR;
    const IV index = PTR2IV(o->op_type);
    SV* keysv;
    HE* he;

    PERL_ARGS_ASSERT_CUSTOM_OP_NAME;

    if (!PL_custom_op_names) /* This probably shouldn't happen */
        return (char *)PL_op_name[OP_CUSTOM];

    keysv = sv_2mortal(newSViv(index));

    he = hv_fetch_ent(PL_custom_op_names, keysv, 0, 0);
    if (!he)
        return (char *)PL_op_name[OP_CUSTOM]; /* Don't know who you are */

    return SvPV_nolen(HeVAL(he));
}

const char*
Perl_custom_op_desc(pTHX_ const OP* o)
{
    dVAR;
    const IV index = PTR2IV(o->op_type);
    SV* keysv;
    HE* he;

    PERL_ARGS_ASSERT_CUSTOM_OP_DESC;

    if (!PL_custom_op_descs)
        return (char *)PL_op_desc[OP_CUSTOM];

    keysv = sv_2mortal(newSViv(index));

    he = hv_fetch_ent(PL_custom_op_descs, keysv, 0, 0);
    if (!he)
        return (char *)PL_op_desc[OP_CUSTOM];

    return SvPV_nolen(HeVAL(he));
}

#include "XSUB.h"

/* Efficient sub that returns a constant scalar value. */
static void
const_sv_xsub(pTHX_ CV* cv)
{
    dVAR;
    dXSARGS;
    SV *const sv = MUTABLE_SV(XSANY.any_ptr);
    if (items != 0) {
	NOOP;
#if 0
	/* diag_listed_as: SKIPME */
        Perl_croak(aTHX_ "usage: %s::%s()",
                   HvNAME_get(GvSTASH(CvGV(cv))), GvNAME(CvGV(cv)));
#endif
    }
    if (!sv) {
	XSRETURN(0);
    }
    EXTEND(sp, 1);
    ST(0) = sv;
    XSRETURN(1);
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
