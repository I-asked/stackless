#include "Python.h"
#include "compile.h"
#include "frameobject.h"
#include "structseq.h"
#include "rotatingtree.h"
#ifdef STACKLESS
#include "stackless_api.h"
#endif

#if !defined(HAVE_LONG_LONG)
#error "This module requires long longs!"
#endif

/*** Selection of a high-precision timer ***/

#ifdef MS_WINDOWS

#include <windows.h>

static PY_LONG_LONG
hpTimer(void)
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

static double
hpTimerUnit(void)
{
    LARGE_INTEGER li;
    if (QueryPerformanceFrequency(&li))
        return 1.0 / li.QuadPart;
    else
        return 0.000001;  /* unlikely */
}

#else  /* !MS_WINDOWS */

#ifndef HAVE_GETTIMEOFDAY
#error "This module requires gettimeofday() on non-Windows platforms!"
#endif

#if (defined(PYOS_OS2) && defined(PYCC_GCC))
#include <sys/time.h>
#else
#include <sys/resource.h>
#include <sys/times.h>
#endif

static PY_LONG_LONG
hpTimer(void)
{
    struct timeval tv;
    PY_LONG_LONG ret;
#ifdef GETTIMEOFDAY_NO_TZ
    gettimeofday(&tv);
#else
    gettimeofday(&tv, (struct timezone *)NULL);
#endif
    ret = tv.tv_sec;
    ret = ret * 1000000 + tv.tv_usec;
    return ret;
}

static double
hpTimerUnit(void)
{
    return 0.000001;
}

#endif  /* MS_WINDOWS */

/************************************************************/
/* Written by Brett Rosen and Ted Czotter */

struct _ProfilerEntry;

/* represents a function called from another function */
typedef struct _ProfilerSubEntry {
    rotating_node_t header;
    PY_LONG_LONG tt;
    PY_LONG_LONG it;
    long callcount;
    long recursivecallcount;
    long recursionLevel;
} ProfilerSubEntry;

/* represents a function or user defined block */
typedef struct _ProfilerEntry {
    rotating_node_t header;
    PyObject *userObj; /* PyCodeObject, or a descriptive str for builtins */
    PY_LONG_LONG tt; /* total time in this entry */
    PY_LONG_LONG it; /* inline time in this entry (not in subcalls) */
    long callcount; /* how many times this was called */
    long recursivecallcount; /* how many times called recursively */
    long recursionLevel;
    rotating_node_t *calls;
} ProfilerEntry;

typedef struct _ProfilerContext {
    PY_LONG_LONG t0;
    PY_LONG_LONG subt;
    PY_LONG_LONG paused;
    struct _ProfilerContext *previous;
    ProfilerEntry *ctxEntry;
    char is_recursion;
    char is_subcall_recursion;
} ProfilerContext;

typedef struct _ProfilerStack {
    rotating_node_t header;
    ProfilerContext *currentProfilerContext;
    PY_LONG_LONG t0;        /* When did stack become non-current? */
} ProfilerStack;

typedef struct {
    PyObject_HEAD
    rotating_node_t *profilerStacks;
    rotating_node_t *profilerEntries;
    ProfilerStack   *currentProfilerStack;
    ProfilerContext *freelistProfilerContext;
    PyObject *externalTimer;
    double externalTimerUnit;
    PY_LONG_LONG currentTime;
    int flags;
    int nProfilerStacks;
} ProfilerObject;

#define CURRENT_CONTEXT(pObj) \
    ((pObj)->currentProfilerStack->currentProfilerContext)

#define POF_ENABLED     0x001
#define POF_SUBCALLS    0x002
#define POF_BUILTINS    0x004
#define POF_NOMEMORY    0x100

staticforward PyTypeObject PyProfiler_Type;

#define PyProfiler_Check(op) PyObject_TypeCheck(op, &PyProfiler_Type)
#define PyProfiler_CheckExact(op) (Py_TYPE(op) == &PyProfiler_Type)

/*** External Timers ***/

#define DOUBLE_TIMER_PRECISION   4294967296.0
static PyObject *empty_tuple;

static PY_LONG_LONG CallExternalTimer(ProfilerObject *pObj)
{
    PY_LONG_LONG result;
    PyObject *o = PyObject_Call(pObj->externalTimer, empty_tuple, NULL);
    if (o == NULL) {
        PyErr_WriteUnraisable(pObj->externalTimer);
        return 0;
    }
    if (pObj->externalTimerUnit > 0.0) {
        /* interpret the result as an integer that will be scaled
           in profiler_getstats() */
        result = PyLong_AsLongLong(o);
    }
    else {
        /* interpret the result as a double measured in seconds.
           As the profiler works with PY_LONG_LONG internally
           we convert it to a large integer */
        double val = PyFloat_AsDouble(o);
        /* error handling delayed to the code below */
        result = (PY_LONG_LONG) (val * DOUBLE_TIMER_PRECISION);
    }
    Py_DECREF(o);
    if (PyErr_Occurred()) {
        PyErr_WriteUnraisable(pObj->externalTimer);
        return 0;
    }
    return result;
}

#define CALL_TIMER(pObj)        ((pObj)->externalTimer ?                \
                                        CallExternalTimer(pObj) :       \
                                        hpTimer())

/*** ProfilerObject ***/

static PyObject *
normalizeUserObj(PyObject *obj)
{
    PyCFunctionObject *fn;
    if (!PyCFunction_Check(obj)) {
        Py_INCREF(obj);
        return obj;
    }
    /* Replace built-in function objects with a descriptive string
       because of built-in methods -- keeping a reference to
       __self__ is probably not a good idea. */
    fn = (PyCFunctionObject *)obj;

    if (fn->m_self == NULL) {
        /* built-in function: look up the module name */
        PyObject *mod = fn->m_module;
        char *modname;
        if (mod && PyString_Check(mod)) {
            modname = PyString_AS_STRING(mod);
        }
        else if (mod && PyModule_Check(mod)) {
            modname = PyModule_GetName(mod);
            if (modname == NULL) {
                PyErr_Clear();
                modname = "__builtin__";
            }
        }
        else {
            modname = "__builtin__";
        }
        if (strcmp(modname, "__builtin__") != 0)
            return PyString_FromFormat("<%s.%s>",
                                       modname,
                                       fn->m_ml->ml_name);
        else
            return PyString_FromFormat("<%s>",
                                       fn->m_ml->ml_name);
    }
    else {
        /* built-in method: try to return
            repr(getattr(type(__self__), __name__))
        */
        PyObject *self = fn->m_self;
        PyObject *name = PyString_FromString(fn->m_ml->ml_name);
        if (name != NULL) {
            PyObject *mo = _PyType_Lookup(Py_TYPE(self), name);
            Py_XINCREF(mo);
            Py_DECREF(name);
            if (mo != NULL) {
                PyObject *res = PyObject_Repr(mo);
                Py_DECREF(mo);
                if (res != NULL)
                    return res;
            }
        }
        PyErr_Clear();
        return PyString_FromFormat("<built-in method %s>",
                                   fn->m_ml->ml_name);
    }
}

static ProfilerEntry*
newProfilerEntry(ProfilerObject *pObj, void *key, PyObject *userObj)
{
    ProfilerEntry *self;
    self = (ProfilerEntry*) malloc(sizeof(ProfilerEntry));
    if (self == NULL) {
        pObj->flags |= POF_NOMEMORY;
        return NULL;
    }
    userObj = normalizeUserObj(userObj);
    if (userObj == NULL) {
        PyErr_Clear();
        free(self);
        pObj->flags |= POF_NOMEMORY;
        return NULL;
    }
    self->header.key = key;
    self->userObj = userObj;
    self->tt = 0;
    self->it = 0;
    self->callcount = 0;
    self->recursivecallcount = 0;
    self->recursionLevel = 0;
    self->calls = EMPTY_ROTATING_TREE;
    RotatingTree_Add(&pObj->profilerEntries, &self->header);
    return self;
}

static ProfilerEntry*
getEntry(ProfilerObject *pObj, void *key)
{
    return (ProfilerEntry*) RotatingTree_Get(&pObj->profilerEntries, key);
}

static ProfilerSubEntry *
getSubEntry(ProfilerObject *pObj, ProfilerEntry *caller, ProfilerEntry* entry)
{
    return (ProfilerSubEntry*) RotatingTree_Get(&caller->calls,
                                                (void *)entry);
}

static ProfilerSubEntry *
newSubEntry(ProfilerObject *pObj,  ProfilerEntry *caller, ProfilerEntry* entry)
{
    ProfilerSubEntry *self;
    self = (ProfilerSubEntry*) malloc(sizeof(ProfilerSubEntry));
    if (self == NULL) {
        pObj->flags |= POF_NOMEMORY;
        return NULL;
    }
    self->header.key = (void *)entry;
    self->tt = 0;
    self->it = 0;
    self->callcount = 0;
    self->recursivecallcount = 0;
    self->recursionLevel = 0;
    RotatingTree_Add(&caller->calls, &self->header);
    return self;
}

static int freeSubEntry(rotating_node_t *header, void *arg)
{
    ProfilerSubEntry *subentry = (ProfilerSubEntry*) header;
    free(subentry);
    return 0;
}

static int freeEntry(rotating_node_t *header, void *arg)
{
    ProfilerEntry *entry = (ProfilerEntry*) header;
    RotatingTree_Enum(entry->calls, freeSubEntry, NULL);
    Py_DECREF(entry->userObj);
    free(entry);
    return 0;
}

static void clearEntries(ProfilerObject *pObj)
{
    RotatingTree_Enum(pObj->profilerEntries, freeEntry, NULL);
    pObj->profilerEntries = EMPTY_ROTATING_TREE;
    /* release the memory hold by the free list of ProfilerContexts */
    while (pObj->freelistProfilerContext) {
        ProfilerContext *c = pObj->freelistProfilerContext;
        pObj->freelistProfilerContext = c->previous;
        free(c);
    }
}

static void
checkRecursion(ProfilerContext *self, int  nStacks, long recursionLevel, long subcallRecursionLevel)
{
    ProfilerEntry *entry = self->ctxEntry;
    ProfilerContext *p;
    self->is_recursion = self->is_subcall_recursion = 0;
    if (subcallRecursionLevel > 1) {
        ProfilerEntry *previous_entry;
        if (nStacks <= 1) {
            self->is_recursion = self->is_subcall_recursion = 1;
            return;
        }
        /* possible subcall recursion (and hence recursion) on this stack. */
        previous_entry = self->previous->ctxEntry;
        for(p = self->previous; p; p = p->previous) {
            if (p->ctxEntry == entry) {
                self->is_recursion = 1;
                if (p->previous && p->previous->ctxEntry == previous_entry) {
                    self->is_subcall_recursion = 1;
                    return;
                }
            }
        }
    }
    else if (recursionLevel > 1) {
        if (nStacks <= 1) {
            self->is_recursion = 1;
            return;
        }
        /* possible regular recurion on this stack.  Check it. */
        for(p = self->previous; p; p = p->previous) {
            if (p->ctxEntry == entry) {
                self->is_recursion = 1;
                return;
            }
        }
    }
}

static void
initContext(ProfilerObject *pObj, ProfilerContext *self, ProfilerEntry *entry)
{
    long subcallRecursionLevel = 0;
    self->ctxEntry = entry;
    self->subt = self->paused = 0;
    self->previous = CURRENT_CONTEXT(pObj);
    CURRENT_CONTEXT(pObj) = self;
    ++entry->recursionLevel;
    if ((pObj->flags & POF_SUBCALLS) && self->previous) {
        /* find or create an entry for me in my caller's entry */
        ProfilerEntry *caller = self->previous->ctxEntry;
        ProfilerSubEntry *subentry = getSubEntry(pObj, caller, entry);
        if (subentry == NULL)
            subentry = newSubEntry(pObj, caller, entry);
        if (subentry)
            subcallRecursionLevel = ++subentry->recursionLevel;
    }
    checkRecursion(self, pObj->nProfilerStacks, entry->recursionLevel,
                   subcallRecursionLevel);
    self->t0 = pObj->currentTime;
}

static void
Stop(ProfilerObject *pObj, ProfilerContext *self, ProfilerEntry *entry)
{
    PY_LONG_LONG tt, it;

    tt = pObj->currentTime - self->t0;
    tt -= self->paused;
    it = tt - self->subt;

    if (self->previous) {
        self->previous->subt += tt;
        self->previous->paused += self->paused;
    }
    CURRENT_CONTEXT(pObj) = self->previous;
    --entry->recursionLevel;
    if (!self->is_recursion)
        entry->tt += tt;
    else
        ++entry->recursivecallcount;
    entry->it += it;
    entry->callcount++;
    if ((pObj->flags & POF_SUBCALLS) && self->previous) {
        /* find or create an entry for me in my caller's entry */
        ProfilerEntry *caller = self->previous->ctxEntry;
        ProfilerSubEntry *subentry = getSubEntry(pObj, caller, entry);
        if (subentry) {
            --subentry->recursionLevel;
            if (!self->is_subcall_recursion)
                subentry->tt += tt;
            else
                ++subentry->recursivecallcount;
            subentry->it += it;
            ++subentry->callcount;
        }
    }
}

static void
ptrace_enter_call(PyObject *self, void *key, PyObject *userObj)
{
    /* entering a call to the function identified by 'key'
       (which can be a PyCodeObject or a PyMethodDef pointer) */
    ProfilerObject *pObj = (ProfilerObject*)self;
    ProfilerEntry *profEntry;
    ProfilerContext *pContext;

    /* In the case of entering a generator expression frame via a
     * throw (gen_send_ex(.., 1)), we may already have an
     * Exception set here. We must not mess around with this
     * exception, and some of the code under here assumes that
     * PyErr_* is its own to mess around with, so we have to
     * save and restore any current exception. */
    PyObject *last_type, *last_value, *last_tb;
    PyErr_Fetch(&last_type, &last_value, &last_tb);

    profEntry = getEntry(pObj, key);
    if (profEntry == NULL) {
        profEntry = newProfilerEntry(pObj, key, userObj);
        if (profEntry == NULL)
            goto restorePyerr;
    }
    /* grab a ProfilerContext out of the free list */
    pContext = pObj->freelistProfilerContext;
    if (pContext) {
        pObj->freelistProfilerContext = pContext->previous;
    }
    else {
        /* free list exhausted, allocate a new one */
        pContext = (ProfilerContext*)
            malloc(sizeof(ProfilerContext));
        if (pContext == NULL) {
            pObj->flags |= POF_NOMEMORY;
            goto restorePyerr;
        }
    }
    initContext(pObj, pContext, profEntry);

restorePyerr:
    PyErr_Restore(last_type, last_value, last_tb);
}

static void
ptrace_leave_call(PyObject *self, void *key)
{
    /* leaving a call to the function identified by 'key' */
    ProfilerObject *pObj = (ProfilerObject*)self;
    ProfilerEntry *profEntry;
    ProfilerContext *pContext;

    pContext = CURRENT_CONTEXT(pObj);
    if (pContext == NULL)
        return;
    profEntry = getEntry(pObj, key);
    if (profEntry) {
        Stop(pObj, pContext, profEntry);
    }
    else {
        CURRENT_CONTEXT(pObj) = pContext->previous;
    }
    /* put pContext into the free list */
    pContext->previous = pObj->freelistProfilerContext;
    pObj->freelistProfilerContext = pContext;
}

static void
SelectStackByKey(ProfilerObject *pObj, void *key)
{
    ProfilerStack *old = pObj->currentProfilerStack;
    ProfilerStack *stack;

    if (old) {
        if (old->header.key == key)
            return;
        old->t0 = pObj->currentTime;
    }
    stack = (ProfilerStack*)RotatingTree_Get(&pObj->profilerStacks, key);
    if (stack) {
        if (stack->currentProfilerContext)
            stack->currentProfilerContext->paused += pObj->currentTime - stack->t0;
    }
    else {
        stack = (ProfilerStack*) malloc(sizeof(ProfilerStack));
        if (stack != NULL) {
            stack->currentProfilerContext = NULL;
            stack->header.key = key;
            RotatingTree_Add(&pObj->profilerStacks, (rotating_node_t*)stack);
            ++pObj->nProfilerStacks;
        }
    }
    pObj->currentProfilerStack = stack;
}

static void
SelectStack(ProfilerObject *pObj)
{
#ifdef STACKLESS
    PyObject *stack = PyStackless_GetCurrent();
    SelectStackByKey(pObj, stack);
    Py_XDECREF(stack);
#else
    PyThreadState *tstate = PyThreadState_GET();
    SelectStackByKey(pObj, tstate);
#endif
}

static int
profiler_callback(PyObject *self, PyFrameObject *frame, int what,
                  PyObject *arg)
{
    ProfilerObject *pObj = (ProfilerObject*)self;
    {
        /* keep error state, see ptrace_enter_call above.
         * We could keep this more focused, only really needed
         * when calling a user time function, and initializing
         * a user object
         */
        PyObject *et, *ev, *tb;
        PyErr_Fetch(&et, &ev, &tb);
        pObj->currentTime = CALL_TIMER(pObj);
        SelectStack(pObj);
        PyErr_Restore(et, ev, tb);
    }
    if (pObj->currentProfilerStack == NULL)
        return 0;

    switch (what) {

    /* the 'frame' of a called function is about to start its execution */
    case PyTrace_CALL:
        ptrace_enter_call(self, (void *)frame->f_code,
                                (PyObject *)frame->f_code);
        break;

    /* the 'frame' of a called function is about to finish
       (either normally or with an exception) */
    case PyTrace_RETURN:
        ptrace_leave_call(self, (void *)frame->f_code);
        break;

    /* case PyTrace_EXCEPTION:
        If the exception results in the function exiting, a
        PyTrace_RETURN event will be generated, so we don't need to
        handle it. */

#ifdef PyTrace_C_CALL   /* not defined in Python <= 2.3 */
    /* the Python function 'frame' is issuing a call to the built-in
       function 'arg' */
    case PyTrace_C_CALL:
        if ((((ProfilerObject *)self)->flags & POF_BUILTINS)
            && PyCFunction_Check(arg)) {
            ptrace_enter_call(self,
                              ((PyCFunctionObject *)arg)->m_ml,
                              arg);
        }
        break;

    /* the call to the built-in function 'arg' is returning into its
       caller 'frame' */
    case PyTrace_C_RETURN:              /* ...normally */
    case PyTrace_C_EXCEPTION:           /* ...with an exception set */
        if ((((ProfilerObject *)self)->flags & POF_BUILTINS)
            && PyCFunction_Check(arg)) {
            ptrace_leave_call(self,
                              ((PyCFunctionObject *)arg)->m_ml);
        }
        break;
#endif

    default:
        break;
    }
    return 0;
}

static int
pending_exception(ProfilerObject *pObj)
{
    if (pObj->flags & POF_NOMEMORY) {
        pObj->flags -= POF_NOMEMORY;
        PyErr_SetString(PyExc_MemoryError,
                        "memory was exhausted while profiling");
        return -1;
    }
    return 0;
}

/************************************************************/

static PyStructSequence_Field profiler_entry_fields[] = {
    {"code",         "code object or built-in function name"},
    {"callcount",    "how many times this was called"},
    {"reccallcount", "how many times called recursively"},
    {"totaltime",    "total time in this entry"},
    {"inlinetime",   "inline time in this entry (not in subcalls)"},
    {"calls",        "details of the calls"},
    {0}
};

static PyStructSequence_Field profiler_subentry_fields[] = {
    {"code",         "called code object or built-in function name"},
    {"callcount",    "how many times this is called"},
    {"reccallcount", "how many times this is called recursively"},
    {"totaltime",    "total time spent in this call"},
    {"inlinetime",   "inline time (not in further subcalls)"},
    {0}
};

static PyStructSequence_Desc profiler_entry_desc = {
    "_lsprof.profiler_entry", /* name */
    NULL, /* doc */
    profiler_entry_fields,
    6
};

static PyStructSequence_Desc profiler_subentry_desc = {
    "_lsprof.profiler_subentry", /* name */
    NULL, /* doc */
    profiler_subentry_fields,
    5
};

static int initialized;
static PyTypeObject StatsEntryType;
static PyTypeObject StatsSubEntryType;


typedef struct {
    PyObject *list;
    PyObject *sublist;
    double factor;
} statscollector_t;

static int statsForSubEntry(rotating_node_t *node, void *arg)
{
    ProfilerSubEntry *sentry = (ProfilerSubEntry*) node;
    statscollector_t *collect = (statscollector_t*) arg;
    ProfilerEntry *entry = (ProfilerEntry*) sentry->header.key;
    int err;
    PyObject *sinfo;
    sinfo = PyObject_CallFunction((PyObject*) &StatsSubEntryType,
                                  "((Olldd))",
                                  entry->userObj,
                                  sentry->callcount,
                                  sentry->recursivecallcount,
                                  collect->factor * sentry->tt,
                                  collect->factor * sentry->it);
    if (sinfo == NULL)
        return -1;
    err = PyList_Append(collect->sublist, sinfo);
    Py_DECREF(sinfo);
    return err;
}

static int statsForEntry(rotating_node_t *node, void *arg)
{
    ProfilerEntry *entry = (ProfilerEntry*) node;
    statscollector_t *collect = (statscollector_t*) arg;
    PyObject *info;
    int err;
    if (entry->callcount == 0)
        return 0;   /* skip */

    if (entry->calls != EMPTY_ROTATING_TREE) {
        collect->sublist = PyList_New(0);
        if (collect->sublist == NULL)
            return -1;
        if (RotatingTree_Enum(entry->calls,
                              statsForSubEntry, collect) != 0) {
            Py_DECREF(collect->sublist);
            return -1;
        }
    }
    else {
        Py_INCREF(Py_None);
        collect->sublist = Py_None;
    }

    info = PyObject_CallFunction((PyObject*) &StatsEntryType,
                                 "((OllddO))",
                                 entry->userObj,
                                 entry->callcount,
                                 entry->recursivecallcount,
                                 collect->factor * entry->tt,
                                 collect->factor * entry->it,
                                 collect->sublist);
    Py_DECREF(collect->sublist);
    if (info == NULL)
        return -1;
    err = PyList_Append(collect->list, info);
    Py_DECREF(info);
    return err;
}

PyDoc_STRVAR(getstats_doc, "\
getstats() -> list of profiler_entry objects\n\
\n\
Return all information collected by the profiler.\n\
Each profiler_entry is a tuple-like object with the\n\
following attributes:\n\
\n\
    code          code object\n\
    callcount     how many times this was called\n\
    reccallcount  how many times called recursively\n\
    totaltime     total time in this entry\n\
    inlinetime    inline time in this entry (not in subcalls)\n\
    calls         details of the calls\n\
\n\
The calls attribute is either None or a list of\n\
profiler_subentry objects:\n\
\n\
    code          called code object\n\
    callcount     how many times this is called\n\
    reccallcount  how many times this is called recursively\n\
    totaltime     total time spent in this call\n\
    inlinetime    inline time (not in further subcalls)\n\
");

static PyObject*
profiler_getstats(ProfilerObject *pObj, PyObject* noarg)
{
    statscollector_t collect;
    if (pending_exception(pObj))
        return NULL;
    if (!pObj->externalTimer)
        collect.factor = hpTimerUnit();
    else if (pObj->externalTimerUnit > 0.0)
        collect.factor = pObj->externalTimerUnit;
    else
        collect.factor = 1.0 / DOUBLE_TIMER_PRECISION;
    collect.list = PyList_New(0);
    if (collect.list == NULL)
        return NULL;
    if (RotatingTree_Enum(pObj->profilerEntries, statsForEntry, &collect)
        != 0) {
        Py_DECREF(collect.list);
        return NULL;
    }
    return collect.list;
}

static int
setSubcalls(ProfilerObject *pObj, int nvalue)
{
    if (nvalue == 0)
        pObj->flags &= ~POF_SUBCALLS;
    else if (nvalue > 0)
        pObj->flags |=  POF_SUBCALLS;
    return 0;
}

static int
setBuiltins(ProfilerObject *pObj, int nvalue)
{
    if (nvalue == 0)
        pObj->flags &= ~POF_BUILTINS;
    else if (nvalue > 0) {
#ifndef PyTrace_C_CALL
        PyErr_SetString(PyExc_ValueError,
                        "builtins=True requires Python >= 2.4");
        return -1;
#else
        pObj->flags |=  POF_BUILTINS;
#endif
    }
    return 0;
}

PyDoc_STRVAR(enable_doc, "\
enable(subcalls=True, builtins=True)\n\
\n\
Start collecting profiling information.\n\
If 'subcalls' is True, also records for each function\n\
statistics separated according to its current caller.\n\
If 'builtins' is True, records the time spent in\n\
built-in functions separately from their caller.\n\
");

static PyObject*
profiler_enable(ProfilerObject *self, PyObject *args, PyObject *kwds)
{
    int subcalls = -1;
    int builtins = -1;
    static char *kwlist[] = {"subcalls", "builtins", 0};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ii:enable",
                                     kwlist, &subcalls, &builtins))
        return NULL;
    if (setSubcalls(self, subcalls) < 0 || setBuiltins(self, builtins) < 0)
        return NULL;
    PyEval_SetProfile(profiler_callback, (PyObject*)self);
    self->flags |= POF_ENABLED;
    Py_INCREF(Py_None);
    return Py_None;
}

static void
flush_unmatched(ProfilerObject *pObj)
{
    if (!pObj->currentProfilerStack)
        return;
    while (CURRENT_CONTEXT(pObj)) {
        ProfilerContext *pContext = CURRENT_CONTEXT(pObj);
        ProfilerEntry *profEntry= pContext->ctxEntry;
        if (profEntry)
            Stop(pObj, pContext, profEntry);
        else
            CURRENT_CONTEXT(pObj) = pContext->previous;
        if (pContext)
            free(pContext);
    }

}

static int
flush_unmatched_enum(rotating_node_t *n, void *arg)
{
    ProfilerStack *t = (ProfilerStack*)n;
    ProfilerObject *pObj = (ProfilerObject*)arg;
    pObj->currentProfilerStack = t;
    flush_unmatched(pObj);
    free(t);
    return 0;
}

static void
flush_unmatched_allstacks(ProfilerObject *pObj)
{
    RotatingTree_Enum(pObj->profilerStacks, &flush_unmatched_enum, pObj);
    pObj->profilerStacks = EMPTY_ROTATING_TREE;
    pObj->currentProfilerStack = NULL;
    pObj->nProfilerStacks = 0;
}

PyDoc_STRVAR(disable_doc, "\
disable()\n\
\n\
Stop collecting profiling information.\n\
");

static PyObject*
profiler_disable(ProfilerObject *self, PyObject* noarg)
{
    self->flags &= ~POF_ENABLED;
    PyEval_SetProfile(NULL, NULL);
    SelectStack(self);
    flush_unmatched(self);
    if (pending_exception(self))
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(clear_doc, "\
clear()\n\
\n\
Clear all profiling information collected so far.\n\
");

static PyObject*
profiler_clear(ProfilerObject *pObj, PyObject* noarg)
{
    flush_unmatched_allstacks(pObj);
    clearEntries(pObj);
    Py_INCREF(Py_None);
    return Py_None;
}

static void
profiler_dealloc(ProfilerObject *op)
{
    if (op->flags & POF_ENABLED)
        PyEval_SetProfile(NULL, NULL);
    flush_unmatched_allstacks(op);
    clearEntries(op);
    Py_XDECREF(op->externalTimer);
    Py_TYPE(op)->tp_free(op);
}

static int
profiler_init(ProfilerObject *pObj, PyObject *args, PyObject *kw)
{
    PyObject *o;
    PyObject *timer = NULL;
    double timeunit = 0.0;
    int subcalls = 1;
#ifdef PyTrace_C_CALL
    int builtins = 1;
#else
    int builtins = 0;
#endif
    static char *kwlist[] = {"timer", "timeunit",
                                   "subcalls", "builtins", 0};

    if (!PyArg_ParseTupleAndKeywords(args, kw, "|Odii:Profiler", kwlist,
                                     &timer, &timeunit,
                                     &subcalls, &builtins))
        return -1;

    if (setSubcalls(pObj, subcalls) < 0 || setBuiltins(pObj, builtins) < 0)
        return -1;
    o = pObj->externalTimer;
    pObj->externalTimer = timer;
    Py_XINCREF(timer);
    Py_XDECREF(o);
    pObj->externalTimerUnit = timeunit;
    return 0;
}

static PyMethodDef profiler_methods[] = {
    {"getstats",    (PyCFunction)profiler_getstats,
                    METH_NOARGS,                        getstats_doc},
    {"enable",          (PyCFunction)profiler_enable,
                    METH_VARARGS | METH_KEYWORDS,       enable_doc},
    {"disable",         (PyCFunction)profiler_disable,
                    METH_NOARGS,                        disable_doc},
    {"clear",           (PyCFunction)profiler_clear,
                    METH_NOARGS,                        clear_doc},
    {NULL, NULL}
};

PyDoc_STRVAR(profiler_doc, "\
Profiler(timer=None, timeunit=None, subcalls=True, builtins=True)\n\
\n\
    Builds a profiler object using the specified timer function.\n\
    The default timer is a fast built-in one based on real time.\n\
    For custom timer functions returning integers, timeunit can\n\
    be a float specifying a scale (i.e. how long each integer unit\n\
    is, in seconds).\n\
");

statichere PyTypeObject PyProfiler_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_lsprof.Profiler",                     /* tp_name */
    sizeof(ProfilerObject),                 /* tp_basicsize */
    0,                                      /* tp_itemsize */
    (destructor)profiler_dealloc,           /* tp_dealloc */
    0,                                      /* tp_print */
    0,                                      /* tp_getattr */
    0,                                      /* tp_setattr */
    0,                                      /* tp_compare */
    0,                                      /* tp_repr */
    0,                                      /* tp_as_number */
    0,                                      /* tp_as_sequence */
    0,                                      /* tp_as_mapping */
    0,                                      /* tp_hash */
    0,                                      /* tp_call */
    0,                                      /* tp_str */
    0,                                      /* tp_getattro */
    0,                                      /* tp_setattro */
    0,                                      /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    profiler_doc,                           /* tp_doc */
    0,                                      /* tp_traverse */
    0,                                      /* tp_clear */
    0,                                      /* tp_richcompare */
    0,                                      /* tp_weaklistoffset */
    0,                                      /* tp_iter */
    0,                                      /* tp_iternext */
    profiler_methods,                       /* tp_methods */
    0,                                      /* tp_members */
    0,                                      /* tp_getset */
    0,                                      /* tp_base */
    0,                                      /* tp_dict */
    0,                                      /* tp_descr_get */
    0,                                      /* tp_descr_set */
    0,                                      /* tp_dictoffset */
    (initproc)profiler_init,                /* tp_init */
    PyType_GenericAlloc,                    /* tp_alloc */
    PyType_GenericNew,                      /* tp_new */
    PyObject_Del,                           /* tp_free */
};

static PyMethodDef moduleMethods[] = {
    {NULL, NULL}
};

PyMODINIT_FUNC
init_lsprof(void)
{
    PyObject *module, *d;
    module = Py_InitModule3("_lsprof", moduleMethods, "Fast profiler");
    if (module == NULL)
        return;
    d = PyModule_GetDict(module);
    if (PyType_Ready(&PyProfiler_Type) < 0)
        return;
    PyDict_SetItemString(d, "Profiler", (PyObject *)&PyProfiler_Type);

    if (!initialized) {
        PyStructSequence_InitType(&StatsEntryType,
                                  &profiler_entry_desc);
        PyStructSequence_InitType(&StatsSubEntryType,
                                  &profiler_subentry_desc);
    }
    Py_INCREF((PyObject*) &StatsEntryType);
    Py_INCREF((PyObject*) &StatsSubEntryType);
    PyModule_AddObject(module, "profiler_entry",
                       (PyObject*) &StatsEntryType);
    PyModule_AddObject(module, "profiler_subentry",
                       (PyObject*) &StatsSubEntryType);
    empty_tuple = PyTuple_New(0);
    initialized = 1;
}