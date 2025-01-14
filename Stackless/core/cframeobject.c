/******************************************************

  The CFrame

 ******************************************************/

/*
 * The purpose of a CFrame is to allow any callable to be run as
 * a tasklet.
 * A CFrame does not appear in tracebacks, but it does
 * play a role in frame chains.
 *
 * For simplicity, it mimicks the fields which slp_transfer needs
 * to do a proper switch, and the standard frame fields have
 * been slightly rearranged, to keep the size of CFrame small.
 * I have looked through all reachable C extensions to find out
 * which fields need to be present.
 *
 * The tasklet holds either a frame or a cframe and knows
 * how to handle them.
 *
 * XXX in a later, thunk-based implementation, CFrames should
 * vanish and be replaced by special thunks, which don't mess
 * with the frame chain.
 */

#include "Python.h"

#ifdef STACKLESS
#include "stackless_impl.h"
#include "pickling/prickelpit.h"

static PyCFrameObject *free_list = NULL;
static int numfree = 0;         /* number of cframes currently in free_list */
#define MAXFREELIST 200         /* max value for numfree */

static void
cframe_dealloc(PyCFrameObject *cf)
{
    PyObject_GC_UnTrack(cf);
    Py_XDECREF(cf->f_back);
    Py_XDECREF(cf->ob1);
    Py_XDECREF(cf->ob2);
    Py_XDECREF(cf->ob3);
    if (numfree < MAXFREELIST) {
        ++numfree;
        cf->f_back = (PyFrameObject *) free_list;
        free_list = cf;
    }
    else
        PyObject_GC_Del(cf);
}

static int
cframe_traverse(PyCFrameObject *cf, visitproc visit, void *arg)
{
    Py_VISIT(cf->f_back);
    Py_VISIT(cf->ob1);
    Py_VISIT(cf->ob2);
    Py_VISIT(cf->ob3);
    return 0;
}

/* clearing a cframe while the object still exists */

static void
cframe_clear(PyCFrameObject *cf)
{
    /* The Python C-API documentation recomends to use Py_CLEAR() to release
     * references held by container objects. The following code is an unrolled
     * version of four Py_CLEAR() macros. It is more robust at no additional
     * costs.
     */
    PyFrameObject *tmp_f_back;
    PyObject *tmp_ob1, *tmp_ob2, *tmp_ob3;
    tmp_f_back = cf->f_back;
    tmp_ob1 = cf->ob1;
    tmp_ob2 = cf->ob2;
    tmp_ob3 = cf->ob3;
    cf->f_back = NULL;
    cf->ob1 = cf->ob2 = cf->ob3 = NULL;
    Py_XDECREF(tmp_f_back);
    Py_XDECREF(tmp_ob1);
    Py_XDECREF(tmp_ob2);
    Py_XDECREF(tmp_ob3);
}


PyCFrameObject *
slp_cframe_new(PyFrame_ExecFunc *exec, unsigned int linked)
{
    PyThreadState *ts = PyThreadState_GET();
    PyCFrameObject *cf;
    PyFrameObject *back;

    if (free_list == NULL) {
        cf = PyObject_GC_NewVar(PyCFrameObject, &PyCFrame_Type, 0);
        if (cf == NULL)
            return NULL;
    }
    else {
        assert(numfree > 0);
        --numfree;
        cf = free_list;
        free_list = (PyCFrameObject *) free_list->f_back;
        _Py_NewReference((PyObject *) cf);
    }

    if (linked) {
        back = SLP_CURRENT_FRAME(ts);
    }
    else
        back = NULL;
    Py_XINCREF(back);
    cf->f_execute = exec;
    cf->f_back = back;
    cf->ob1 = cf->ob2 = cf->ob3 = NULL;
    cf->i = cf->n = 0;
    cf->any1 = cf->any2 = NULL;
    _PyObject_GC_TRACK(cf);
    return cf;
}

/* pickling support for cframes */

#define cframetuplefmt "iSOll"
#define cframetuplenewfmt "iSO!ll:cframe"


static PyObject *
cframe_reduce(PyCFrameObject *cf)
{
    PyObject *res = NULL, *exec_name = NULL;
    PyObject *params = NULL;
    int valid = 1;

    if ((exec_name = slp_find_execname((PyFrameObject *) cf, &valid)) == NULL)
        return NULL;

    params = slp_into_tuple_with_nulls(&cf->ob1, 3);
    if (params == NULL) goto err_exit;

    res = Py_BuildValue ("(O()(" cframetuplefmt "))",
                         cf->ob_type,
                         valid,
                         exec_name,
                         params,
                         cf->i,
                         cf->n);

err_exit:
    Py_XDECREF(exec_name);
    Py_XDECREF(params);
    return res;
}

static PyObject *
cframe_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, ":cframe", kwlist))
        return NULL;
    return (PyObject *) slp_cframe_new(NULL, 0);
}

/* note that args is a tuple, although we use METH_O */

static PyObject *
cframe_setstate(PyObject *self, PyObject *args)
{
    PyCFrameObject *cf = (PyCFrameObject *) self;
    int valid;
    PyObject *exec_name = NULL;
    PyFrame_ExecFunc *good_func, *bad_func;
    PyObject *params;
    long i, n;

    if (!PyArg_ParseTuple (args, cframetuplenewfmt,
                           &valid,
                           &exec_name,
                           &PyTuple_Type, &params,
                           &i,
                           &n))
        return NULL;

    if (slp_find_execfuncs(cf->ob_type, exec_name, &good_func, &bad_func))
        return NULL;

    if (PyTuple_GET_SIZE(params)-1 != 3)
        VALUE_ERROR("bad argument for cframe unpickling", NULL);

    /* mark this frame as coming from unpickling */
    Py_INCREF(Py_None);
    cf->f_back = (PyFrameObject *) Py_None;
    cf->f_execute = valid ? good_func : bad_func;
    slp_from_tuple_with_nulls(&cf->ob1, params);
    cf->i = i;
    cf->n = n;
    cf->any1 = cf->any2 = NULL;
    Py_INCREF(cf);
    return (PyObject *) cf;
}

static PyMethodDef cframe_methods[] = {
    {"__reduce__",        (PyCFunction)cframe_reduce, METH_NOARGS, NULL},
    {"__reduce_ex__", (PyCFunction)cframe_reduce, METH_VARARGS, NULL},
    {"__setstate__",  (PyCFunction)cframe_setstate, METH_O, NULL},
    {NULL, NULL}
};


static PyObject * run_cframe(PyFrameObject *f, int exc, PyObject *retval)
{
    PyThreadState *ts = PyThreadState_GET();
    PyCFrameObject *cf = (PyCFrameObject*) f;
    PyTaskletObject *task = ts->st.current;
    int done = cf->i;

    SLP_SET_CURRENT_FRAME(ts, f);

    if (retval == NULL || done)
        goto exit_run_cframe;

    if (cf->ob2 == NULL)
        cf->ob2 = PyTuple_New(0);
    Py_DECREF(retval);
    STACKLESS_PROPOSE_ALL();
    retval = PyObject_Call(cf->ob1, cf->ob2, cf->ob3);
    STACKLESS_ASSERT();
    cf->i = 1; /* mark ourself as done */

    if (STACKLESS_UNWINDING(retval)) {
        /* try to shortcut */
        PyFrameObject *f;  /* a borrowed ref */
        if (ts->st.current == task && (f = SLP_PEEK_NEXT_FRAME(ts)) != NULL &&
            f->f_back == (PyFrameObject *) cf) {
                Py_XINCREF(cf->f_back);
                Py_SETREF(f->f_back, cf->f_back);
        }
        return retval;
    }
    /* pop frame */
exit_run_cframe:
    SLP_STORE_NEXT_FRAME(ts, cf->f_back);
    return retval;
}

DEF_INVALID_EXEC(run_cframe)

PyCFrameObject *
slp_cframe_newfunc(PyObject *func, PyObject *args, PyObject *kwds, unsigned int linked)
{
    PyCFrameObject *cf;

    if (func == NULL || !PyCallable_Check(func))
        TYPE_ERROR("cframe function must be a callable", NULL);
    cf = slp_cframe_new(run_cframe, linked);
    if (cf == NULL)
        return NULL;
    Py_INCREF(func);
    cf->ob1 = func;
    Py_INCREF(args);
    cf->ob2 = args;
    Py_XINCREF(kwds);
    cf->ob3 = kwds;
    return cf;
}

static PyMemberDef cframe_memberlist[] = {
    {"f_back",          T_OBJECT,   offsetof(PyCFrameObject, f_back),   RO},
    {"ob1",             T_OBJECT,   offsetof(PyCFrameObject, ob1),      RO},
    {"ob2",             T_OBJECT,   offsetof(PyCFrameObject, ob2),      RO},
    {"ob3",             T_OBJECT,   offsetof(PyCFrameObject, ob3),      RO},
    {"i",               T_LONG,     offsetof(PyCFrameObject, i),        RO},
    {"n",               T_LONG,     offsetof(PyCFrameObject, n),        RO},
    {NULL}  /* Sentinel */
};

PyTypeObject PyCFrame_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "_stackless.cframe",
    sizeof(PyCFrameObject),
    0,
    (destructor)cframe_dealloc,                 /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    PyObject_GenericSetAttr,                    /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)cframe_traverse,              /* tp_traverse */
    (inquiry) cframe_clear,                     /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    cframe_methods,                             /* tp_methods */
    cframe_memberlist,                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    cframe_new,                                 /* tp_new */
    _PyObject_Del,                              /* tp_free */
};

int init_cframetype(void)
{
    if (PyType_Ready(&PyCFrame_Type))
        return -1;
    /* register the cframe exec func */
    return slp_register_execute(&PyCFrame_Type, "run_cframe",
                                run_cframe, REF_INVALID_EXEC(run_cframe));
}

/* Clear out the free list */

void
slp_cframe_fini(void)
{
    while (free_list != NULL) {
        PyCFrameObject *cf = free_list;
        free_list = (PyCFrameObject *) free_list->f_back;
        PyObject_GC_Del(cf);
        --numfree;
    }
    assert(numfree == 0);
}

#endif
