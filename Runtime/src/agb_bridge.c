/*
 * Script bridge, game side (see agb_bridge.h). Once per frame, on the game's thread:
 *   - (re)publishes the variable / request descriptions when tables were added:
 *       variables "name\ttype\tcount\tstride\thelp\n", requests "name\thelp\n"
 *   - runs the requests the host queued (built-in "set <name>" value[, index])
 *   - hands the host a copy of every variable's bytes, in table order
 * through four host imports (agb_host.h), implemented by the native host (editor child
 * process: shared memory) and the wasm bridge (in-process: AgbHostApi).
 */
#include "agb_bridge.h"
#include "agb_host.h"

#include <string.h>

#define MAX_TABLES 16
#define TEXT_CAP 16384
#define REQ_TEXT_CAP 4096
#define VALUES_CAP 32768

typedef struct Table
{
    const AgbBridgeVar *vars;
    int nvars;
    const AgbBridgeRequest *requests;
    int nrequests;
} Table;

static Table sTables[MAX_TABLES];
static int sNumTables;
static int sDirty;
static char sVarText[TEXT_CAP];
static char sReqText[REQ_TEXT_CAP];
static unsigned char sValues[VALUES_CAP];

void agb_bridge_add(const AgbBridgeVar *vars, int nvars, const AgbBridgeRequest *requests, int nrequests)
{
    if (sNumTables >= MAX_TABLES) return;
    sTables[sNumTables].vars = vars;
    sTables[sNumTables].nvars = vars ? nvars : 0;
    sTables[sNumTables].requests = requests;
    sTables[sNumTables].nrequests = requests ? nrequests : 0;
    sNumTables++;
    sDirty = 1;
}

static int element_size(int type)
{
    switch (type)
    {
    case AGB_VAR_U8:
    case AGB_VAR_S8:
    case AGB_VAR_STR: return 1;
    case AGB_VAR_U16:
    case AGB_VAR_S16: return 2;
    default: return 4;
    }
}

/* bytes the variable takes in the values block */
static int var_bytes(const AgbBridgeVar *v)
{
    if (v->type == AGB_VAR_STR) return v->stride ? v->count * v->stride : v->count;
    return v->count * element_size(v->type);
}

static char *append(char *p, char *end, const char *s)
{
    while (*s && p < end - 1) *p++ = *s++;
    *p = 0;
    return p;
}

static char *append_int(char *p, char *end, int value)
{
    char buf[16];
    int n = 0;
    unsigned u = value < 0 ? (unsigned)-value : (unsigned)value;

    do
    {
        buf[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u && n < 15);
    if (value < 0 && p < end - 1) *p++ = '-';
    while (n && p < end - 1) *p++ = buf[--n];
    *p = 0;
    return p;
}

static void publish(void)
{
    char *p = sVarText, *end = sVarText + TEXT_CAP;
    char *q = sReqText, *qend = sReqText + REQ_TEXT_CAP;
    int t, i;

    sVarText[0] = 0;
    sReqText[0] = 0;
    for (t = 0; t < sNumTables; t++)
    {
        for (i = 0; i < sTables[t].nvars; i++)
        {
            const AgbBridgeVar *v = &sTables[t].vars[i];
            p = append(p, end, v->name);
            p = append(p, end, "\t");
            p = append_int(p, end, v->type);
            p = append(p, end, "\t");
            p = append_int(p, end, v->count);
            p = append(p, end, "\t");
            p = append_int(p, end, v->stride);
            p = append(p, end, "\t");
            p = append(p, end, v->help ? v->help : "");
            p = append(p, end, "\n");
        }
        for (i = 0; i < sTables[t].nrequests; i++)
        {
            const AgbBridgeRequest *r = &sTables[t].requests[i];
            q = append(q, qend, r->name);
            q = append(q, qend, "\t");
            q = append(q, qend, r->help ? r->help : "");
            q = append(q, qend, "\n");
        }
    }
    agb_host_bridge_publish(sVarText, sReqText);
}

static const AgbBridgeVar *find_var(const char *name)
{
    int t, i;

    for (t = 0; t < sNumTables; t++)
        for (i = 0; i < sTables[t].nvars; i++)
            if (strcmp(sTables[t].vars[i].name, name) == 0) return &sTables[t].vars[i];
    return 0;
}

static int set_var(const char *name, const int *args, int nargs)
{
    const AgbBridgeVar *v = find_var(name);
    unsigned char *p;
    int index;

    if (v == 0 || v->type == AGB_VAR_STR) return AGB_BRIDGE_RESULT_UNKNOWN;
    if (nargs < 1) return AGB_BRIDGE_RESULT_BAD_ARGS;
    index = nargs > 1 ? args[1] : 0;
    if (index < 0 || index >= v->count) return AGB_BRIDGE_RESULT_BAD_ARGS;
    p = (unsigned char *)v->addr + index * (v->stride ? v->stride : element_size(v->type));
    switch (element_size(v->type))
    {
    case 1: *p = (unsigned char)args[0]; break;
    case 2: *(unsigned short *)p = (unsigned short)args[0]; break;
    default: *(unsigned int *)p = (unsigned int)args[0]; break;
    }
    return 0;
}

static int run_request(const char *name, const int *args, int nargs)
{
    int t, i;

    if (strncmp(name, "set ", 4) == 0) return set_var(name + 4, args, nargs);
    for (t = 0; t < sNumTables; t++)
        for (i = 0; i < sTables[t].nrequests; i++)
            if (strcmp(sTables[t].requests[i].name, name) == 0)
                return sTables[t].requests[i].fn(args, nargs);
    return AGB_BRIDGE_RESULT_UNKNOWN;
}

void agb_bridge_pump(void)
{
    char name[64];
    int args[AGB_BRIDGE_MAX_ARGS];
    int nargs, id, t, i, guard;
    unsigned size = 0;

    if (sNumTables == 0) return;
    if (sDirty)
    {
        sDirty = 0;
        publish();
    }
    for (guard = 0; guard < 32; guard++)
    {
        nargs = 0;
        id = agb_host_bridge_poll(name, sizeof(name), args, AGB_BRIDGE_MAX_ARGS, &nargs);
        if (id <= 0) break;
        name[sizeof(name) - 1] = 0;
        agb_host_bridge_done(id, run_request(name, args, nargs));
    }
    /* the variables' bytes, in table order */
    for (t = 0; t < sNumTables; t++)
    {
        for (i = 0; i < sTables[t].nvars; i++)
        {
            const AgbBridgeVar *v = &sTables[t].vars[i];
            const int bytes = var_bytes(v);
            const int elem = element_size(v->type);
            if (size + (unsigned)bytes > VALUES_CAP) break;
            if (v->type == AGB_VAR_STR || v->stride == 0 || v->stride == elem)
            {
                memcpy(sValues + size, v->addr, (size_t)bytes);
            }
            else
            {
                int k;
                for (k = 0; k < v->count; k++)
                    memcpy(sValues + size + k * elem, (const unsigned char *)v->addr + k * v->stride, (size_t)elem);
            }
            size += (unsigned)bytes;
        }
    }
    agb_host_bridge_values(sValues, size);
}
