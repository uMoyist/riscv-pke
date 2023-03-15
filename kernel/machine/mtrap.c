#include "kernel/riscv.h"
#include "kernel/process.h"
#include "spike_interface/spike_utils.h"
#include "util/string.h"

static void handle_instruction_access_fault() { panic("Instruction access fault!"); }

static void handle_load_access_fault() { panic("Load access fault!"); }

static void handle_store_access_fault() { panic("Store/AMO access fault!"); }

static void handle_illegal_instruction() { panic("Illegal instruction!"); }

static void handle_misaligned_load() { panic("Misaligned Load!"); }

static void handle_misaligned_store() { panic("Misaligned AMO!"); }

// added @lab1_3
static void handle_timer() {
  int cpuid = 0;
  // setup the timer fired at next time (TIMER_INTERVAL from now)
  *(uint64*)CLINT_MTIMECMP(cpuid) = *(uint64*)CLINT_MTIMECMP(cpuid) + TIMER_INTERVAL;

  // setup a soft interrupt in sip (S-mode Interrupt Pending) to be handled in S-mode
  write_csr(sip, SIP_SSIP);
}

static void print_exinfo() {
  int i;
  addr_line* line = current->line;
  code_file* file = current->file;
  char** dir = current->dir;
  uint64 mepc = read_csr(mepc);
  sprint("%x\n",mepc);
  for(i = 0; i < current->line_ind; i++)
  {
    sprint("%x %s\n", line[i].addr, file[line[i].file].file);
    if (line[i].addr == mepc) break;
  }
  int l = line[i].line;
  char *filename = file[line[i].file].file;
  char *dirname = dir[file[line[i].file].dir];
  // sprint("%d %s\n",i,filename);

  int len1 = -1;
  while (dirname[++len1])
    ;

  int len2 = -1;
  while (filename[++len2])
    ;
  char path[len1 + len2 + 2];

  int x = -1;
  while ((++x) < len1)
    path[x] = dirname[x];
  path[x] = '/';
  

  while ((++x) < len2+len1+1)
    path[x] = filename[x-len1-1];

  path[x] = 0;

  // strcpy(path, dirname);

  // strcpy(path+len1+1,filename);

  sprint("Runtime error at %s:%lld\n", path, l);

  spike_file_t *fp = spike_file_open(path, O_RDONLY, 0);
  i = 0;
  char c;
  while(i<l-1) {
    while(spike_file_read(fp,&c,1)&&c!='\n');
    i++;
  }
  char sen[256];
  i = 0;

  while (spike_file_read(fp, &c, 1) && c != '\n')
    sen[i++] = c;
  spike_file_close(fp);
  sen[i] = 0;
  sprint("%s\n",sen);
}

//
// handle_mtrap calls a handling function according to the type of a machine mode interrupt (trap).
//
void handle_mtrap() {
  uint64 mcause = read_csr(mcause);
  if(mcause != CAUSE_MTIMER)
    print_exinfo();
  switch (mcause) {
    case CAUSE_MTIMER:
      handle_timer();
      break;
    case CAUSE_FETCH_ACCESS:
      handle_instruction_access_fault();
      break;
    case CAUSE_LOAD_ACCESS:
      handle_load_access_fault();
    case CAUSE_STORE_ACCESS:
      handle_store_access_fault();
      break;
    case CAUSE_ILLEGAL_INSTRUCTION:
      // TODO (lab1_2): call handle_illegal_instruction to implement illegal instruction
      // interception, and finish lab1_2.
      // panic( "call handle_illegal_instruction to accomplish illegal instruction interception for lab1_2.\n" );
      handle_illegal_instruction();
      break;
    case CAUSE_MISALIGNED_LOAD:
      handle_misaligned_load();
      break;
    case CAUSE_MISALIGNED_STORE:
      handle_misaligned_store();
      break;

    default:
      sprint("machine trap(): unexpected mscause %p\n", mcause);
      sprint("            mepc=%p mtval=%p\n", read_csr(mepc), read_csr(mtval));
      panic( "unexpected exception happened in M-mode.\n" );
      break;
  }
}
