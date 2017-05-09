/* MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "mruby.h"
#include "mruby/string.h"
#include "process.h"

#include <windows.h>
#include <process.h>

#define MAXCHILDNUM 256 /* max num of child processes */
#define P_OVERLAY 2

/* License: Ruby's */
static struct ChildRecord {
    HANDLE hProcess;
    pid_t pid;
} ChildRecord[MAXCHILDNUM];

/* License: Ruby's */
#define FOREACH_CHILD(v) do { \
    struct ChildRecord* v; \
    for (v = ChildRecord; v < ChildRecord + sizeof(ChildRecord) / sizeof(ChildRecord[0]); ++v)
#define END_FOREACH_CHILD } while (0)

static FARPROC get_proc_address(const char *module, const char *func, HANDLE *mh);
static pid_t poll_child_status(struct ChildRecord *child, int *stat_loc);
static struct ChildRecord *FindChildSlot(pid_t pid);
static struct ChildRecord *FindChildSlotByHandle(HANDLE h);
static void CloseChildHandle(struct ChildRecord *child);
static struct ChildRecord *
CreateChild(const WCHAR *cmd, const WCHAR *prog, SECURITY_ATTRIBUTES *psa,
	    HANDLE hInput, HANDLE hOutput, HANDLE hError, DWORD dwCreationFlags);
static struct ChildRecord * FindFreeChildSlot(void);
static pid_t child_result(struct ChildRecord *child, int mode);

int
fork(void)
{
    return -1;
}

/* License: Ruby's */
pid_t
getppid(void)
{
    typedef long (WINAPI query_func)(HANDLE, int, void *, ULONG, ULONG *);
    static query_func *pNtQueryInformationProcess = (query_func *) - 1;
    pid_t ppid = 0;

    if (pNtQueryInformationProcess == (query_func *) - 1)
        pNtQueryInformationProcess = (query_func *)get_proc_address("ntdll.dll", "NtQueryInformationProcess", NULL);

    if (pNtQueryInformationProcess) {
        struct {
            long ExitStatus;
            void* PebBaseAddress;
            uintptr_t AffinityMask;
            uintptr_t BasePriority;
            uintptr_t UniqueProcessId;
            uintptr_t ParentProcessId;
        } pbi;

        ULONG len;
        long ret = pNtQueryInformationProcess(GetCurrentProcess(), 0, &pbi, sizeof(pbi), &len);

        if (!ret)
            ppid = pbi.ParentProcessId;
    }

    return ppid;
}

/* License: Artistic or GPL */
pid_t
waitpid(pid_t pid, int *stat_loc, int options)
{
    DWORD timeout;
    struct ChildRecord* child;
    int count, retried, ret;

    /* Artistic or GPL part start */
    if (options == WNOHANG)
        timeout = 0;
    else
        timeout = INFINITE;
    /* Artistic or GPL part end */

    if (pid == -1) {
        HANDLE targets[MAXCHILDNUM];
        struct ChildRecord* cause;

        count = 0;

        FOREACH_CHILD(child) {
            if (!child->pid || child->pid < 0) continue;
            if ((pid = poll_child_status(child, stat_loc))) return pid;
            targets[count++] = child->hProcess;
        } END_FOREACH_CHILD;

        if (!count)
            return -1;

        ret = WaitForMultipleObjects(count, targets, FALSE, timeout);
        if (ret == WAIT_TIMEOUT) return 0;
        if ((ret -= WAIT_OBJECT_0) == count) return -1;
        if (ret > count) return -1;

        cause = FindChildSlotByHandle(targets[ret]);
        if (!cause) return -1;

        return poll_child_status(cause, stat_loc);
    }
    else {
        child   = FindChildSlot(pid);
        retried = 0;

        if (!child || child->hProcess == INVALID_HANDLE_VALUE)
            return -1;

        while (!(pid = poll_child_status(child, stat_loc))) {
            /* wait... */
            ret = WaitForMultipleObjects(1, &child->hProcess, FALSE, timeout);

            if (ret == WAIT_OBJECT_0 + 1) return -1; /* maybe EINTR */
            if (ret != WAIT_OBJECT_0) {
                /* still active */
                if (options & WNOHANG) {
                    pid = 0;
                    break;
                }
                ++retried;
            }
        }

        if (pid == -1 && retried) pid = 0;
    }

    return pid;
}

/* License: Ruby's */
int
kill(pid_t pid, int sig)
{
    pid_t ret = 0;
    DWORD ctrlEvent;
    HANDLE hProc;
    struct ChildRecord* child;

    if (pid < 0 || (pid == 0 && sig != SIGINT))
        return -1;

    if ((unsigned int)pid == GetCurrentProcessId() && (sig != 0 && sig != SIGKILL)) {
        ret = raise(sig);
        return ret;
    }

    switch (sig) {
        case 0:
            hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, (DWORD)pid);

            if (hProc == NULL || hProc == INVALID_HANDLE_VALUE) {
                ret = -1;
            }
            else {
                CloseHandle(hProc);
            }

            break;

        case SIGINT:
            ctrlEvent = CTRL_C_EVENT;

            if (pid != 0) ctrlEvent = CTRL_BREAK_EVENT;
            if (!GenerateConsoleCtrlEvent(ctrlEvent, (DWORD)pid)) ret = -1;

            break;

        case SIGKILL:
            child = FindChildSlot(pid);

            if (child) {
                hProc = child->hProcess;
            }
            else {
                hProc = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION, FALSE, (DWORD)pid);
            }

            if (hProc == NULL || hProc == INVALID_HANDLE_VALUE) {
                ret = -1;
            }
            else {
                DWORD status;

                if (!GetExitCodeProcess(hProc, &status)) {
                    ret = -1;
                }
                else if (status == STILL_ACTIVE) {
                    if (!TerminateProcess(hProc, 0)) {
                        ret = -1;
                    }
                }
                else {
                    ret = -1;
                }

                if (!child) {
                    CloseHandle(hProc);
                }
            }

            break;

    default:
        ret = -1;
  }

  return ret;
}

/* License: Ruby's */
static pid_t
poll_child_status(struct ChildRecord *child, int *stat_loc)
{
    DWORD exitcode;

    if (!GetExitCodeProcess(child->hProcess, &exitcode)) {
        /* If an error occurred, return immediately. */
    error_exit:
        CloseChildHandle(child);
        return -1;
    }

    if (exitcode != STILL_ACTIVE) {
        pid_t pid;

        /* If already died, wait process's real termination. */
        if (WaitForSingleObject(child->hProcess, INFINITE) != WAIT_OBJECT_0) {
            goto error_exit;
        }

        pid = child->pid;
        CloseChildHandle(child);

        if (stat_loc)
            *stat_loc = exitcode << 8;

        return pid;
    }

    return 0;
}

/* License: Ruby's */
static struct ChildRecord *
FindChildSlot(pid_t pid)
{
    FOREACH_CHILD(child) {
        if (pid == -1 || child->pid == pid)
            return child;
    } END_FOREACH_CHILD;

    return NULL;
}

/* License: Ruby's */
static struct ChildRecord *
FindChildSlotByHandle(HANDLE h)
{
    FOREACH_CHILD(child) {
        if (child->hProcess == h)
            return child;
    } END_FOREACH_CHILD;

    return NULL;
}

/* License: Ruby's */
static void
CloseChildHandle(struct ChildRecord *child)
{
    HANDLE h        = child->hProcess;
    child->hProcess = NULL;
    child->pid      = 0;

    CloseHandle(h);
}

/* License: Ruby's */
static FARPROC
get_proc_address(const char *module, const char *func, HANDLE *mh)
{
    HANDLE h;
    FARPROC ptr;

    if (mh)
      h = LoadLibrary(module);
    else
      h = GetModuleHandle(module);
    if (!h)
      return NULL;

    ptr = GetProcAddress(h, func);
    if (mh) {
      if (ptr)
        *mh = h;
      else
        FreeLibrary(h);
    }
    return ptr;
}

mrb_value
mrb_argv0(mrb_state *mrb)
{
    TCHAR argv0[MAX_PATH + 1];

    GetModuleFileName(NULL, argv0, MAX_PATH + 1);

    return mrb_str_new_cstr(mrb, argv0);
}

mrb_value
mrb_progname(mrb_state *mrb)
{
    TCHAR argv0[MAX_PATH + 1];
    char *progname;

    GetModuleFileName(NULL, argv0, MAX_PATH + 1);

    progname = strrchr(argv0, '\\');

    if (progname)
        progname++;
    else
        progname = argv0;

    return mrb_str_new_cstr(mrb, progname);
}

int
spawnv(pid_t *pid, const char *path, char *const argv[])
{
  return 0; // TODO
}

int
spawnve(pid_t *pid, const char *path, char *const argv[], char *const envp[])
{
  return 0; // TODO
}

int
spawn(int mode, const char *cmd, const char *prog, int cp)
{


  const char *shell = NULL;
  WCHAR *wcmd = NULL, *wshell = NULL;
  int e = 0;
  int ret = -1;

  shell = prog;
  // wshell = mbstr_to_wstr(cp, shell, -1, NULL);
  // wcmd = mbstr_to_wstr(cp, cmd, -1, NULL);

  ret = child_result(CreateChild(wcmd, wshell, NULL, NULL, NULL, NULL, 0), mode);

  free(wshell);
  free(wcmd);
  if (e) errno = e;
  return ret;
}

static pid_t
child_result(struct ChildRecord *child, int mode)
{
    DWORD exitcode;

    if (!child) {
	     return -1;
    }

    if (mode == P_OVERLAY) {
    	WaitForSingleObject(child->hProcess, INFINITE);
    	GetExitCodeProcess(child->hProcess, &exitcode);
    	CloseChildHandle(child);
    	_exit(exitcode);
    }
    return child->pid;
}

static struct ChildRecord *
FindFreeChildSlot(void)
{
    FOREACH_CHILD(child) {
	     if (!child->pid) {
  	      child->pid = -1;	/* lock the slot */
  	       child->hProcess = NULL;
  	        return child;
	     }
    } END_FOREACH_CHILD;
    return NULL;
}

static struct ChildRecord *
CreateChild(const WCHAR *cmd, const WCHAR *prog, SECURITY_ATTRIBUTES *psa,
	    HANDLE hInput, HANDLE hOutput, HANDLE hError, DWORD dwCreationFlags)
{
    BOOL fRet;
    STARTUPINFOW aStartupInfo;
    PROCESS_INFORMATION aProcessInformation;
    SECURITY_ATTRIBUTES sa;
    struct ChildRecord *child;

    if (!cmd && !prog) {
    	return NULL;
    }

    child = FindFreeChildSlot();
    if (!child) {
	     return NULL;
    }

    if (!psa) {
    	sa.nLength              = sizeof (SECURITY_ATTRIBUTES);
    	sa.lpSecurityDescriptor = NULL;
    	sa.bInheritHandle       = TRUE;
    	psa = &sa;
    }

    memset(&aStartupInfo, 0, sizeof(aStartupInfo));
    memset(&aProcessInformation, 0, sizeof(aProcessInformation));
    aStartupInfo.cb = sizeof(aStartupInfo);
    aStartupInfo.dwFlags = STARTF_USESTDHANDLES;
    if (hInput) {
	     aStartupInfo.hStdInput  = hInput;
    }
    else {
	     aStartupInfo.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    }
    if (hOutput) {
	     aStartupInfo.hStdOutput = hOutput;
    }
    else {
	     aStartupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (hError) {
	     aStartupInfo.hStdError = hError;
    }
    else {
	     aStartupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }

    dwCreationFlags |= NORMAL_PRIORITY_CLASS;

    if (lstrlenW(cmd) > 32767) {
    	child->pid = 0;		/* release the slot */
    	return NULL;
    }

    fRet = CreateProcessW(prog, (WCHAR *)cmd, psa, psa,
                          psa->bInheritHandle, dwCreationFlags, NULL, NULL,
                          &aStartupInfo, &aProcessInformation);


    if (!fRet) {
    	child->pid = 0;		/* release the slot */
    	return NULL;
    }

    CloseHandle(aProcessInformation.hThread);

    child->hProcess = aProcessInformation.hProcess;
    child->pid = (pid_t)aProcessInformation.dwProcessId;

    return child;
}
