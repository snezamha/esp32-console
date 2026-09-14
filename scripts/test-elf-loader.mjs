import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
const directory = mkdtempSync(join(tmpdir(), "esp32-elf-test-"));
const root = resolve("firmware/esp32-s3-lcd-0.85/src/runtime/elf_loader");
const source = readFileSync(join(root, "esp_elf.c"), "utf8");
const extract = (text, marker) => {
  const start = text.indexOf(marker), braces = text.indexOf("{", start);
  if (start < 0) throw new Error(`Missing loader function: ${marker}`);
  let depth = 1, end = braces + 1;
  while (depth && end < text.length) { if (text[end] === "{") depth++; if (text[end] === "}") depth--; end++; }
  return text.slice(start, end);
};
const allocation = extract(readFileSync(join(root, "esp_elf_adapter.c"), "utf8"), "void *esp_elf_malloc(");
const program = `
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include "${root}/loader_config.h"
#include "${root}/private/elf_types.h"
#define ELF_ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define stype(s,t) ((s)->type==(t))
#define sflags(s,f) (((s)->flags&(f))==(f))
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_EXEC 2
#define MALLOC_CAP_8BIT 4
size_t allocation_size; uint32_t allocation_caps;
void *heap_caps_malloc(size_t size, uint32_t caps) { allocation_size=size;allocation_caps=caps;return malloc(size); }
void esp_elf_free(void *p) { free(p); }
${allocation}
${extract(source, "static int esp_elf_load_section(")}
${extract(source, "uintptr_t esp_elf_map_sym(")}
int main(int argc, char **argv) {
 void *p=esp_elf_malloc(7,true);assert(allocation_size==8);assert(allocation_caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_EXEC));free(p);
 for(int n=1;n<argc;n++) {
  FILE *f=fopen(argv[n],"rb");assert(f);fseek(f,0,SEEK_END);size_t length=ftell(f);rewind(f);
  uint8_t *bytes=malloc(length);assert(fread(bytes,1,length,f)==length);fclose(f);
  elf32_hdr_t *header=(elf32_hdr_t *)bytes;
  elf32_shdr_t *sections=(elf32_shdr_t *)(bytes+header->shoff);
  const char *names=(const char *)bytes+sections[header->shstrndx].offset;
  esp_elf_t elf={0};assert(!esp_elf_load_section(&elf,bytes));
  for(int i=0;i<header->shnum;i++) {
   const elf32_shdr_t *s=&sections[i];
   if(!strcmp(names+s->name,".text")) {
    assert(elf.sec[ELF_SEC_TEXT].size==s->size);
    assert(!memcmp(elf.ptext,bytes+s->offset,s->size));
    for(size_t j=s->size;j<((s->size+3)&~3u);j++)assert(elf.ptext[j]==0);
   }
   if(!strcmp(names+s->name,".rodata")) {
    for(size_t j=0;j<s->size;j++)assert(esp_elf_map_sym(&elf,s->addr+j)==elf.sec[ELF_SEC_RODATA].addr+j);
   }
   if(s->type==SHT_RELA) {
    for(size_t j=0;j<s->size;j+=sizeof(elf32_rela_t)) {
     elf32_rela_t *rel=(elf32_rela_t *)(bytes+s->offset+j);uint32_t target=0;
     for(int k=0;k<header->shnum;k++)if(rel->offset>=sections[k].addr&&rel->offset-sections[k].addr+4<=sections[k].size&&sections[k].type==SHT_PROGBITS) { memcpy(&target,bytes+sections[k].offset+rel->offset-sections[k].addr,4);break; }
     assert(esp_elf_map_sym(&elf,target)!=0);
    }
   }
  }
  free(elf.ptext);free(elf.pdata);free(bytes);
 }
 puts("ELF loader preserves virtual section boundaries, rodata targets and padded IRAM allocation");
}
`;
try {
  writeFileSync(join(directory, "sdkconfig.h"), "");
  const test = join(directory, "test.c"), binary = join(directory, "test");
  writeFileSync(test, program);
  execFileSync(process.env.CC || "cc", ["-std=c11", "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast", "-I", directory, test, "-o", binary], { stdio: "inherit" });
  const projects = JSON.parse(readFileSync("projects/manifest.json", "utf8")).projects;
  execFileSync(binary, projects.map((p) => resolve(`public${p.path}`)), { stdio: "inherit" });
} finally { rmSync(directory, { recursive: true, force: true }); }
