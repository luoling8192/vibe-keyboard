#include "vk_vka1.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *bytes, uint16_t value) { bytes[0]=(uint8_t)value; bytes[1]=(uint8_t)(value>>8U); }
static void put32(uint8_t *bytes, uint32_t value) { bytes[0]=(uint8_t)value; bytes[1]=(uint8_t)(value>>8U); bytes[2]=(uint8_t)(value>>16U); bytes[3]=(uint8_t)(value>>24U); }

static uint8_t *load(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb"); assert(file);
    assert(fseek(file, 0, SEEK_END) == 0); long size = ftell(file); assert(size > 0); rewind(file);
    uint8_t *bytes = malloc((size_t)size); assert(bytes); assert(fread(bytes, 1, (size_t)size, file) == (size_t)size); fclose(file); *length = (size_t)size; return bytes;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *names[] = {"one-pixel", "equal-raw", "full-run", "row-boundary", "mixed"};
    vk_vka1_limits_t limits = {.max_frames=8,.min_frame_ms=10,.max_frame_ms=1000,.max_container_bytes=1000000,.max_decoded_bytes=428u*142u*2u};
    for (size_t i=0;i<sizeof(names)/sizeof(names[0]);i++) {
        char path[1024]; assert(snprintf(path,sizeof(path),"%s/%s.vka1",argv[1],names[i])>0);
        size_t length; uint8_t *bytes=load(path,&length); vk_vka1_info_t info;
        assert(vk_vka1_validate(bytes,length,&limits,&info)==VK_VKA1_OK);
        size_t count=(size_t)info.width*info.height; uint16_t *pixels=calloc(count,sizeof(*pixels)); assert(pixels);
        for(uint16_t f=0;f<info.frame_count;f++)assert(vk_vka1_decode_frame(bytes,length,f,pixels,count)==VK_VKA1_OK);
        uint8_t *mutated=malloc(length);assert(mutated);memcpy(mutated,bytes,length);mutated[24]^=1;assert(vk_vka1_validate(mutated,length,&limits,&info)==VK_VKA1_HASH);
        assert(vk_vka1_validate(bytes,length-1,&limits,&info)!=VK_VKA1_OK);
        const size_t fixed[] = {0U,1U,2U,3U,4U,5U,6U,7U,8U,10U,12U,14U,16U,20U,23U,24U,55U,56U,60U,64U,67U,length-1U};
        for(size_t m=0;m<sizeof(fixed)/sizeof(fixed[0]);++m){if(fixed[m]>=length)continue;memcpy(mutated,bytes,length);mutated[fixed[m]]^=(uint8_t)(1U<<(m&7U));assert(vk_vka1_validate(mutated,length,&limits,&info)!=VK_VKA1_OK);}
        memcpy(mutated,bytes,length);put16(mutated+14U,0U);assert(vk_vka1_validate(mutated,length,&limits,&info)!=VK_VKA1_OK);
        memcpy(mutated,bytes,length);put32(mutated+16U,UINT32_MAX);assert(vk_vka1_validate(mutated,length,&limits,&info)!=VK_VKA1_OK);
        memcpy(mutated,bytes,length);put32(mutated+20U,UINT32_MAX);assert(vk_vka1_validate(mutated,length,&limits,&info)!=VK_VKA1_OK);
        free(mutated);free(pixels);free(bytes);
    }
    char vector_path[1024], fixture_path[1024], line[128];
    assert(snprintf(vector_path,sizeof(vector_path),"%s/mutations-v1.csv",argv[1])>0);
    assert(snprintf(fixture_path,sizeof(fixture_path),"%s/mixed.vka1",argv[1])>0);
    size_t fixture_length;uint8_t *fixture=load(fixture_path,&fixture_length),*mutation=malloc(fixture_length);assert(mutation);
    FILE *vectors=fopen(vector_path,"rb");assert(vectors);assert(fgets(line,sizeof(line),vectors));unsigned cases=0;
    while(fgets(line,sizeof(line),vectors)){char name[16],expected[16];size_t offset;unsigned bit;assert(sscanf(line,"%15[^,],%zu,%u,%15s",name,&offset,&bit,expected)==4);assert(!strcmp(name,"mixed")&&!strcmp(expected,"reject")&&offset<fixture_length&&bit<8U);memcpy(mutation,fixture,fixture_length);mutation[offset]^=(uint8_t)(1U<<bit);vk_vka1_info_t info;assert(vk_vka1_validate(mutation,fixture_length,&limits,&info)!=VK_VKA1_OK);++cases;}
    assert(fclose(vectors)==0&&cases==512U);free(mutation);free(fixture);
    puts("VKA1 native tests passed");
    return 0;
}
