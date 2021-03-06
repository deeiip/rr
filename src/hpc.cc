/* -*- Mode: C++; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#include "hpc.h"

#include <assert.h>
#include <fcntl.h>
#include <perfmon/pfmlib_perf_event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <fstream>

#include "log.h"
#include "task.h"

using namespace std;

/**
 * libpfm4 specific stuff
 */
void init_libpfm()
{
	int ret = pfm_initialize();
	if (ret != PFM_SUCCESS) {
		FATAL() <<"Failed to init libpfm: "<< pfm_strerror(ret);
	}
}

void close_libpfm()
{
	pfm_terminate();
}

static void libpfm_event_encoding(struct perf_event_attr* attr,
				  const char* event_str, int hw_event)
{
	memset(attr, 0, sizeof(struct perf_event_attr));

	int ret;
	char* fstr;

	attr->size = sizeof(struct perf_event_attr);
	ret = pfm_get_perf_event_encoding(event_str, PFM_PLM0, attr, &fstr, NULL);
	if (ret != PFM_SUCCESS) {
		errx(1, "error while encoding event string %s :%s\n", event_str, pfm_strerror(ret));
	}
	if (hw_event && attr->type != PERF_TYPE_RAW) {
		errx(1, "error: %s is not a raw hardware event\n", event_str);
	}
	free(fstr);
}


enum cpuid_requests {
  CPUID_GETVENDORSTRING,
  CPUID_GETFEATURES,
  CPUID_GETTLB,
  CPUID_GETSERIAL,

  CPUID_INTELEXTENDED=0x80000000,
  CPUID_INTELFEATURES,
  CPUID_INTELBRANDSTRING,
  CPUID_INTELBRANDSTRINGMORE,
  CPUID_INTELBRANDSTRINGEND,
};

/** issue a single request to CPUID. Fits 'intel features', for instance
 *  note that even if only "eax" and "edx" are of interest, other registers
 *  will be modified by the operation, so we need to tell the compiler about it.
 */
static void cpuid(int code, unsigned int *a, unsigned int *d) {
/* this asm returns 1 if CPUID is supported, 0 otherwise (ZF is also set accordingly)
 * add it later for full compatibility

	pushfd ; get
	pop eax
	mov ecx, eax ; save
	xor eax, 0x200000 ; flip
	push eax ; set
	popfd
	pushfd ; and test
	pop eax
	xor eax, ecx ; mask changed bits
	shr eax, 21 ; move bit 21 to bit 0
	and eax, 1 ; and mask others
	push ecx
	popfd ; restore original flags
	ret

 */
  asm volatile("cpuid":"=a"(*a),"=d"(*d):"a"(code):"ecx","ebx");
}

/*
 * Find out the cpu model using the cpuid instruction.
 * Full list of CPUIDs at http://sandpile.org/x86/cpuid.htm
 * Another list at http://software.intel.com/en-us/articles/intel-architecture-and-processor-identification-with-cpuid-model-and-family-numbers
 */
enum cpu_type {
	UnknownCpu,
	IntelMerom,
	IntelPenryn,
	IntelNehalem,
	IntelWestmere,
	IntelSandyBridge,
	IntelIvyBridge,
	IntelHaswell
};
static cpu_type get_cpu_type()
{
	unsigned int cpu_type,eax,edx;
	cpuid(CPUID_GETFEATURES,&eax,&edx);
	cpu_type = (eax & 0xF0FF0);
	switch (cpu_type) {
	case 0x006F0:
	case 0x10660:
		return IntelMerom;
	case 0x10670:
	case 0x106D0:
		return IntelPenryn;
	case 0x106A0:
	case 0x106E0:
	case 0x206E0:
		return IntelNehalem;
	case 0x20650:
	case 0x206C0:
	case 0x206F0:
		return IntelWestmere;
	case 0x206A0:
	case 0x206D0:
		return IntelSandyBridge;
	case 0x306A0:
		return IntelIvyBridge;
	case 0x306C0:
	case 0x40660:
		return IntelHaswell;
	default:
		FATAL() << "CPU "<< HEX(cpu_type) << " unknown.";
		return UnknownCpu; // not reached
	}
}

void init_hpc(Task* t)
{
	struct hpc_context* counters =
		(struct hpc_context*)calloc(1, sizeof(*counters));
	t->hpc = counters;

	/* get the event that counts down to the initial value
	 * the precision level enables PEBS support. precise=0 uses the counter
	 * with PEBS disabled */
	const char * rbc_event = 0;
	const char * inst_event = 0;
	const char * hw_int_event = 0;
	const char * page_faults_event = "PERF_COUNT_SW_PAGE_FAULTS:u";
	switch (get_cpu_type()) {
	case IntelMerom :
		FATAL() <<"Intel Merom CPUs currently unsupported.";
		break;
	case IntelPenryn :
		FATAL() <<"Intel Penryn CPUs currently unsupported.";
		break;
	case IntelWestmere :
	case IntelNehalem :
		rbc_event = "BR_INST_RETIRED:CONDITIONAL:u:precise=0";
		inst_event = "INST_RETIRED:u";
		hw_int_event = "BR_INST_RETIRED:CONDITIONAL:u:precise=0";
		break;
	case IntelSandyBridge :
		rbc_event = "BR_INST_RETIRED:CONDITIONAL:u:precise=0";
		inst_event = "INST_RETIRED:u";
		hw_int_event = "HW_INTERRUPTS:u";
		break;
	case IntelIvyBridge :
		rbc_event = "BR_INST_RETIRED:COND:u:precise=0";
		inst_event = "INST_RETIRED:u";
		hw_int_event = "HW_INTERRUPTS:u";
		break;
	case IntelHaswell : {
		rbc_event = "BR_INST_RETIRED:CONDITIONAL:u:precise=0";
		inst_event = "INST_RETIRED:u";
		hw_int_event = "HW_INTERRUPTS:u";
		break;
	}
	default:
		FATAL() <<"Unknown CPU type";
	}

	libpfm_event_encoding(&(counters->rbc.attr), rbc_event , 1);
#ifdef HPC_ENABLE_EXTRA_PERF_COUNTERS
	libpfm_event_encoding(&(counters->inst.attr), inst_event , 1);
	/* counts up to double check */
	//libpfm_event_encoding(&(counters->rbc.attr), rbc_event, 1);
	libpfm_event_encoding(&(counters->hw_int.attr), hw_int_event, 1);
	//libpfm_event_encoding(&(counters->hw_int.attr), event_str, 1);
	libpfm_event_encoding(&(counters->page_faults.attr), page_faults_event, 0);
#else
	(void)inst_event;
	(void)hw_int_event;
	(void)page_faults_event;
#endif
}

static void start_counter(Task* t, int group_fd, hpc_event_t* counter)
{
	counter->fd = syscall(__NR_perf_event_open, &counter->attr, t->tid,
			      -1, group_fd, 0);
	if (0 > counter->fd) {
		FATAL() <<"Failed to initialize counter";
	}
	if (ioctl(counter->fd, PERF_EVENT_IOC_ENABLE, 0)) {
		FATAL() <<"Failed to start counter";
	}
}

static void stop_counter(Task* t, const hpc_event_t* counter)
{
	if (ioctl(counter->fd, PERF_EVENT_IOC_DISABLE, 0)) {
		FATAL() <<"Failed to stop counter";
	}
}

static void __start_hpc(Task* t)
{
	struct hpc_context *counters = t->hpc;
	pid_t tid = t->tid;

	start_counter(t, -1, &counters->rbc);
	counters->group_leader = counters->rbc.fd;

#ifdef HPC_ENABLE_EXTRA_PERF_COUNTERS
	start_counter(t, counters->group_leader, &counters->hw_int);
	start_counter(t, counters->group_leader, &counters->inst);
	start_counter(t, counters->group_leader, &counters->page_faults);
#endif

	struct f_owner_ex own;
	own.type = F_OWNER_TID;
	own.pid = tid;
	if (fcntl(counters->rbc.fd, F_SETOWN_EX, &own)) {
		FATAL() <<"Failed to SETOWN_EX rbc event fd";
	}
	if (fcntl(counters->rbc.fd, F_SETFL, O_ASYNC)
	    || fcntl(counters->rbc.fd, F_SETSIG, HPC_TIME_SLICE_SIGNAL)) {
		FATAL() <<"Failed to make rbc counter ASYNC with sig"
			<< signalname(HPC_TIME_SLICE_SIGNAL);
	}

	counters->started = true;
}

void stop_hpc(Task* t)
{
	struct hpc_context* counters = t->hpc;
	if (!counters->started) {
		return;
	}

	stop_counter(t, &counters->rbc);
#ifdef HPC_ENABLE_EXTRA_PERF_COUNTERS
	stop_counter(t, &counters->hw_int);
	stop_counter(t, &counters->inst);
	stop_counter(t, &counters->page_faults);
#endif
}

static void cleanup_hpc(Task* t)
{
	struct hpc_context* counters = t->hpc;

	stop_hpc(t);

	close(counters->rbc.fd);
#ifdef HPC_ENABLE_EXTRA_PERF_COUNTERS
	close(counters->hw_int.fd);
	close(counters->inst.fd);
	close(counters->page_faults.fd);
#endif
	counters->started = false;
}

/*
 * Starts the hpc.
 * @param t: the current execution context
 * @param reset: the counters are (if enabled) reset
 */
void start_hpc(Task *t, int64_t val)
{
	t->hpc->rbc.attr.sample_period = val;
	__start_hpc(t);
}

void reset_hpc(Task *t, int64_t val)
{
	if (t->hpc->started) {
		cleanup_hpc(t);
	}
	t->hpc->rbc.attr.sample_period = val;
	__start_hpc(t);
}
/**
 * Ultimately frees all resources that are used by hpc of the corresponding
 * t. After calling this function, counters cannot be used anymore
 */
void destroy_hpc(Task *t)
{
	struct hpc_context* counters = t->hpc;
	cleanup_hpc(t);
	free(counters);
}

static int64_t read_counter(struct hpc_context* hpc, int fd)
{
	if (!hpc->started) {
		return 0;
	}
	int64_t val;
	ssize_t nread = read(fd, &val, sizeof(val));
	assert(nread == sizeof(val));
	return val;
}

int64_t read_rbc(struct hpc_context* hpc)
{
	return read_counter(hpc, hpc->rbc.fd);
}

#ifdef HPC_ENABLE_EXTRA_PERF_COUNTERS
int64_t read_hw_int(struct hpc_context* hpc)
{
	return read_counter(hpc, hpc->hw_int.fd);
}

int64_t read_insts(struct hpc_context* hpc)
{
	return read_counter(hpc, hpc->inst.fd);
}


int64_t read_page_faults(struct hpc_context* hpc)
{
	return read_counter(hpc, hpc->page_faults.fd);
}
#endif
