#ifndef _CPU_LDS_SYMBOL_H_
#define _CPU_LDS_SYMBOL_H_

#define __VECTORS_SIZE  0x80
extern unsigned int __bss_start;
extern unsigned int __bss_end;
extern unsigned int __exception_section_start;
extern unsigned int _user_stack_start;
extern unsigned int _user_stack_end;
extern unsigned int _user_heap_start;
extern unsigned int _user_heap_end;
extern unsigned int __text_start;
extern unsigned int __text_end;
extern unsigned int __param_start;
extern unsigned int __param_stop;
extern unsigned int __start;
extern unsigned int _mapped_rtosdata_size;

extern char __func_symbol_index_start[];
extern char __func_symbol_index_end[];
extern unsigned int __func_symbol_str_start;
extern unsigned int __func_symbol_str_end;

extern unsigned int __ksymtab_start;
extern unsigned int __ksymtab_end;

#define TEXT_SEGMENT_START (__text_start-__VECTORS_SIZE)  // Adjust based on actual memory layout
#define TEXT_SEGMENT_END   __text_end  // Adjust based on actual memory layout

#define IS_IN_TEXT_SEGMENT(addr) ((addr) >= TEXT_SEGMENT_START && (addr) <= TEXT_SEGMENT_END)

#define IS_HALFWORD_ALIGNED(addr)  (((addr)&(0x1))==0)
#define IS_WORD_ALIGNED(addr)  (((addr)&(0x3))==0)

#endif /*  _CPU_LDS_SYMBOL_H_ */
