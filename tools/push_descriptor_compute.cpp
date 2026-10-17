// Vulkan 1.0 compute regression covering multiple descriptor sets, sparse
// bindings, descriptor arrays, split descriptor writes, and partial push constants.
//
// The embedded SPIR-V was generated and validated with:
//   glslc --target-env=vulkan1.0 -O push_descriptor_compute.comp -o shader.spv
//   spirv-val --target-env vulkan1.0 shader.spv
// GLSL:
//   #version 450
//   layout(local_size_x = 8) in;
//   layout(set=0,binding=0,std430) readonly buffer In { uint v[]; } input_data;
//   layout(set=0,binding=3,std430) writeonly buffer Out { uint v[]; } output_data;
//   layout(set=1,binding=2,std430) readonly buffer Table { uint v[]; } tables[3];
//   layout(push_constant) uniform Constants {
//       uint multiplier; uint increment; uint xor_mask; uint rotate;
//   } constants;
//   void main() {
//       uint i = gl_GlobalInvocationID.x;
//       uint mixed = input_data.v[i] * constants.multiplier + constants.increment;
//       mixed = (mixed << constants.rotate) | (mixed >> (32u-constants.rotate));
//       uint table_mix = tables[0].v[i] ^ tables[1].v[i] ^ tables[2].v[i];
//       output_data.v[i] = mixed ^ table_mix ^ constants.xor_mask;
//   }

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define VK_CHECK(call) do { VkResult r_=(call); if (r_ != VK_SUCCESS) { \
 fprintf(stderr,"FATAL: %s failed with VkResult=%d\n",#call,int(r_)); exit(1); } } while(0)

static constexpr uint32_t kCount=64, kLocalSize=8;
static constexpr uint32_t kExpectedChecksum=0xf2abf38au;
static const uint32_t kSpirv[] = {
    0x07230203,0x00010000,0x000d000b,0x00000054,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0006000f,0x00000005,0x00000004,0x6e69616d,0x00000000,0x0000000b,0x00060010,0x00000004,
    0x00000011,0x00000008,0x00000001,0x00000001,0x00040047,0x0000000b,0x0000000b,0x0000001c,
    0x00040047,0x00000011,0x00000006,0x00000004,0x00030047,0x00000012,0x00000003,0x00040048,
    0x00000012,0x00000000,0x00000018,0x00050048,0x00000012,0x00000000,0x00000023,0x00000000,
    0x00030047,0x00000014,0x00000018,0x00040047,0x00000014,0x00000021,0x00000000,0x00040047,
    0x00000014,0x00000022,0x00000000,0x00030047,0x0000001b,0x00000002,0x00050048,0x0000001b,
    0x00000000,0x00000023,0x00000000,0x00050048,0x0000001b,0x00000001,0x00000023,0x00000004,
    0x00050048,0x0000001b,0x00000002,0x00000023,0x00000008,0x00050048,0x0000001b,0x00000003,
    0x00000023,0x0000000c,0x00040047,0x00000033,0x00000006,0x00000004,0x00030047,0x00000034,
    0x00000003,0x00040048,0x00000034,0x00000000,0x00000018,0x00050048,0x00000034,0x00000000,
    0x00000023,0x00000000,0x00030047,0x00000038,0x00000018,0x00040047,0x00000038,0x00000021,
    0x00000002,0x00040047,0x00000038,0x00000022,0x00000001,0x00040047,0x00000045,0x00000006,
    0x00000004,0x00030047,0x00000046,0x00000003,0x00040048,0x00000046,0x00000000,0x00000019,
    0x00050048,0x00000046,0x00000000,0x00000023,0x00000000,0x00030047,0x00000048,0x00000019,
    0x00040047,0x00000048,0x00000021,0x00000003,0x00040047,0x00000048,0x00000022,0x00000000,
    0x00040047,0x00000053,0x0000000b,0x00000019,0x00020013,0x00000002,0x00030021,0x00000003,
    0x00000002,0x00040015,0x00000006,0x00000020,0x00000000,0x00040017,0x00000009,0x00000006,
    0x00000003,0x00040020,0x0000000a,0x00000001,0x00000009,0x0004003b,0x0000000a,0x0000000b,
    0x00000001,0x0004002b,0x00000006,0x0000000c,0x00000000,0x00040020,0x0000000d,0x00000001,
    0x00000006,0x0003001d,0x00000011,0x00000006,0x0003001e,0x00000012,0x00000011,0x00040020,
    0x00000013,0x00000002,0x00000012,0x0004003b,0x00000013,0x00000014,0x00000002,0x00040015,
    0x00000015,0x00000020,0x00000001,0x0004002b,0x00000015,0x00000016,0x00000000,0x00040020,
    0x00000018,0x00000002,0x00000006,0x0006001e,0x0000001b,0x00000006,0x00000006,0x00000006,
    0x00000006,0x00040020,0x0000001c,0x00000009,0x0000001b,0x0004003b,0x0000001c,0x0000001d,
    0x00000009,0x00040020,0x0000001e,0x00000009,0x00000006,0x0004002b,0x00000015,0x00000022,
    0x00000001,0x0004002b,0x00000015,0x00000027,0x00000003,0x0004002b,0x00000006,0x0000002c,
    0x00000020,0x0003001d,0x00000033,0x00000006,0x0003001e,0x00000034,0x00000033,0x0004002b,
    0x00000006,0x00000035,0x00000003,0x0004001c,0x00000036,0x00000034,0x00000035,0x00040020,
    0x00000037,0x00000002,0x00000036,0x0004003b,0x00000037,0x00000038,0x00000002,0x0004002b,
    0x00000015,0x00000040,0x00000002,0x0003001d,0x00000045,0x00000006,0x0003001e,0x00000046,
    0x00000045,0x00040020,0x00000047,0x00000002,0x00000046,0x0004003b,0x00000047,0x00000048,
    0x00000002,0x0004002b,0x00000006,0x00000051,0x00000008,0x0004002b,0x00000006,0x00000052,
    0x00000001,0x0006002c,0x00000009,0x00000053,0x00000051,0x00000052,0x00000052,0x00050036,
    0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x00050041,0x0000000d,
    0x0000000e,0x0000000b,0x0000000c,0x0004003d,0x00000006,0x0000000f,0x0000000e,0x00060041,
    0x00000018,0x00000019,0x00000014,0x00000016,0x0000000f,0x0004003d,0x00000006,0x0000001a,
    0x00000019,0x00050041,0x0000001e,0x0000001f,0x0000001d,0x00000016,0x0004003d,0x00000006,
    0x00000020,0x0000001f,0x00050084,0x00000006,0x00000021,0x0000001a,0x00000020,0x00050041,
    0x0000001e,0x00000023,0x0000001d,0x00000022,0x0004003d,0x00000006,0x00000024,0x00000023,
    0x00050080,0x00000006,0x00000025,0x00000021,0x00000024,0x00050041,0x0000001e,0x00000028,
    0x0000001d,0x00000027,0x0004003d,0x00000006,0x00000029,0x00000028,0x000500c4,0x00000006,
    0x0000002a,0x00000025,0x00000029,0x00050082,0x00000006,0x0000002f,0x0000002c,0x00000029,
    0x000500c2,0x00000006,0x00000030,0x00000025,0x0000002f,0x000500c5,0x00000006,0x00000031,
    0x0000002a,0x00000030,0x00070041,0x00000018,0x0000003a,0x00000038,0x00000016,0x00000016,
    0x0000000f,0x0004003d,0x00000006,0x0000003b,0x0000003a,0x00070041,0x00000018,0x0000003d,
    0x00000038,0x00000022,0x00000016,0x0000000f,0x0004003d,0x00000006,0x0000003e,0x0000003d,
    0x000500c6,0x00000006,0x0000003f,0x0000003b,0x0000003e,0x00070041,0x00000018,0x00000042,
    0x00000038,0x00000040,0x00000016,0x0000000f,0x0004003d,0x00000006,0x00000043,0x00000042,
    0x000500c6,0x00000006,0x00000044,0x0000003f,0x00000043,0x000500c6,0x00000006,0x0000004c,
    0x00000031,0x00000044,0x00050041,0x0000001e,0x0000004d,0x0000001d,0x00000040,0x0004003d,
    0x00000006,0x0000004e,0x0000004d,0x000500c6,0x00000006,0x0000004f,0x0000004c,0x0000004e,
    0x00060041,0x00000018,0x00000050,0x00000048,0x00000016,0x0000000f,0x0003003e,0x00000050,
    0x0000004f,0x000100fd,0x00010038,
};

struct Buffer { VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; bool coherent=false; };

static uint32_t memoryType(VkPhysicalDevice physical,uint32_t bits,bool& coherent) {
 VkPhysicalDeviceMemoryProperties p{}; vkGetPhysicalDeviceMemoryProperties(physical,&p);
 const VkMemoryPropertyFlags choices[]={VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT};
 for(auto flags:choices) for(uint32_t i=0;i<p.memoryTypeCount;++i) if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&flags)==flags) {
  coherent=(p.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0; return i;
 }
 fprintf(stderr,"FATAL: no host-visible memory type\n"); exit(1);
}
static uint32_t inputValue(uint32_t i) { return i*0x9e3779b9u^0xa5a5a5a5u; }
static uint32_t tableValue(uint32_t table,uint32_t i) {
 static constexpr uint32_t mul[3]={0x85ebca6bu,0xc2b2ae35u,0x27d4eb2fu};
 static constexpr uint32_t mask[3]={0xc3c3c3c3u,0x5a5a5a5au,0x3c3c3c3cu};
 return i*mul[table]^mask[table];
}
static uint32_t expectedValue(uint32_t i) {
 uint32_t mixed=inputValue(i)*1664525u+1013904223u;
 mixed=(mixed<<7)|(mixed>>25);
 return mixed^tableValue(0,i)^tableValue(1,i)^tableValue(2,i)^0x6a09e667u;
}
static uint32_t checksum(const uint32_t* values) {
 uint32_t hash=2166136261u;
 for(uint32_t i=0;i<kCount;++i) for(uint32_t shift=0;shift<32;shift+=8) { hash^=(values[i]>>shift)&0xffu; hash*=16777619u; }
 return hash;
}

int main(int argc,char** argv) {
 bool validate=true;
 for(int i=1;i<argc;++i) {
  if(strcmp(argv[i],"--no-validate")==0) validate=false; else if(strcmp(argv[i],"--validate")==0) validate=true;
  else { fprintf(stderr,"usage: %s [--validate|--no-validate]\n",argv[0]); return 2; }
 }
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName="push-descriptor-compute"; app.apiVersion=VK_API_VERSION_1_0;
 std::vector<const char*> layers;
 if(validate) { uint32_t n=0; vkEnumerateInstanceLayerProperties(&n,nullptr); std::vector<VkLayerProperties> available(n); vkEnumerateInstanceLayerProperties(&n,available.data()); for(const auto& l:available) if(strcmp(l.layerName,"VK_LAYER_KHRONOS_validation")==0) layers.push_back("VK_LAYER_KHRONOS_validation"); }
 VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app; ici.enabledLayerCount=uint32_t(layers.size()); ici.ppEnabledLayerNames=layers.data();
 VkInstance instance; VK_CHECK(vkCreateInstance(&ici,nullptr,&instance));
 uint32_t physicalCount=0; VK_CHECK(vkEnumeratePhysicalDevices(instance,&physicalCount,nullptr)); if(!physicalCount) { fprintf(stderr,"FATAL: no Vulkan physical device\n"); return 1; }
 std::vector<VkPhysicalDevice> physicals(physicalCount); VK_CHECK(vkEnumeratePhysicalDevices(instance,&physicalCount,physicals.data())); VkPhysicalDevice physical=physicals[0];
 uint32_t familyCount=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&familyCount,nullptr); std::vector<VkQueueFamilyProperties> families(familyCount); vkGetPhysicalDeviceQueueFamilyProperties(physical,&familyCount,families.data());
 uint32_t family=UINT32_MAX; for(uint32_t i=0;i<familyCount;++i) if(families[i].queueFlags&VK_QUEUE_COMPUTE_BIT) { family=i; break; } if(family==UINT32_MAX) { fprintf(stderr,"FATAL: no compute queue\n"); return 1; }
 float priority=1; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex=family; qci.queueCount=1; qci.pQueuePriorities=&priority;
 VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci; VkDevice device; VK_CHECK(vkCreateDevice(physical,&dci,nullptr,&device)); VkQueue queue; vkGetDeviceQueue(device,family,0,&queue);
 VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(physical,&props); const VkDeviceSize size=sizeof(uint32_t)*kCount;
 std::array<Buffer,5> buffers{};
 for(auto& item:buffers) {
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=size; bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE; VK_CHECK(vkCreateBuffer(device,&bi,nullptr,&item.buffer));
  VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device,item.buffer,&req); VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=memoryType(physical,req.memoryTypeBits,item.coherent); VK_CHECK(vkAllocateMemory(device,&ai,nullptr,&item.memory)); VK_CHECK(vkBindBufferMemory(device,item.buffer,item.memory,0));
 }
 for(uint32_t b=0;b<5;++b) {
  void* mapped=nullptr; VK_CHECK(vkMapMemory(device,buffers[b].memory,0,VK_WHOLE_SIZE,0,&mapped)); auto* v=static_cast<uint32_t*>(mapped);
  for(uint32_t i=0;i<kCount;++i) v[i]=b==0?inputValue(i):(b>=2?tableValue(b-2,i):0xdeadbeefu);
  if(!buffers[b].coherent) { VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory=buffers[b].memory; range.offset=0; range.size=VK_WHOLE_SIZE; VK_CHECK(vkFlushMappedMemoryRanges(device,1,&range)); }
  vkUnmapMemory(device,buffers[b].memory);
 }
 std::array<VkDescriptorSetLayoutBinding,2> set0Bindings{};
 set0Bindings[0]={0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}; set0Bindings[1]={3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
 VkDescriptorSetLayoutBinding set1Binding{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
 VkDescriptorSetLayoutCreateInfo sl0{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; sl0.bindingCount=uint32_t(set0Bindings.size()); sl0.pBindings=set0Bindings.data();
 VkDescriptorSetLayoutCreateInfo sl1{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; sl1.bindingCount=1; sl1.pBindings=&set1Binding;
 std::array<VkDescriptorSetLayout,2> layouts{}; VK_CHECK(vkCreateDescriptorSetLayout(device,&sl0,nullptr,&layouts[0])); VK_CHECK(vkCreateDescriptorSetLayout(device,&sl1,nullptr,&layouts[1]));
 VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,5}; VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets=2; dpi.poolSizeCount=1; dpi.pPoolSizes=&poolSize; VkDescriptorPool pool; VK_CHECK(vkCreateDescriptorPool(device,&dpi,nullptr,&pool));
 VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool=pool; dai.descriptorSetCount=2; dai.pSetLayouts=layouts.data(); std::array<VkDescriptorSet,2> sets{}; VK_CHECK(vkAllocateDescriptorSets(device,&dai,sets.data()));
 std::array<VkDescriptorBufferInfo,5> infos{}; for(uint32_t i=0;i<5;++i) infos[i]={buffers[i].buffer,0,size};
 std::array<VkWriteDescriptorSet,3> firstWrites{};
 firstWrites[0]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,sets[0],0,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&infos[0],nullptr};
 firstWrites[1]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,sets[0],3,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&infos[1],nullptr};
 firstWrites[2]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,sets[1],2,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&infos[2],nullptr};
 vkUpdateDescriptorSets(device,uint32_t(firstWrites.size()),firstWrites.data(),0,nullptr);
 VkWriteDescriptorSet secondWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; secondWrite.dstSet=sets[1]; secondWrite.dstBinding=2; secondWrite.dstArrayElement=1; secondWrite.descriptorCount=2; secondWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; secondWrite.pBufferInfo=&infos[3]; vkUpdateDescriptorSets(device,1,&secondWrite,0,nullptr);
 VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize=sizeof(kSpirv); smi.pCode=kSpirv; VkShaderModule shader; VK_CHECK(vkCreateShaderModule(device,&smi,nullptr,&shader));
 VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT,0,16}; VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount=2; pli.pSetLayouts=layouts.data(); pli.pushConstantRangeCount=1; pli.pPushConstantRanges=&pushRange; VkPipelineLayout pipelineLayout; VK_CHECK(vkCreatePipelineLayout(device,&pli,nullptr,&pipelineLayout));
 VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; stage.module=shader; stage.pName="main"; VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pci.stage=stage; pci.layout=pipelineLayout; VkPipeline pipeline; VK_CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pci,nullptr,&pipeline));
 VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpci.queueFamilyIndex=family; VkCommandPool commandPool; VK_CHECK(vkCreateCommandPool(device,&cpci,nullptr,&commandPool)); VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cbai.commandPool=commandPool; cbai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount=1; VkCommandBuffer cb; VK_CHECK(vkAllocateCommandBuffers(device,&cbai,&cb));
 VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; VK_CHECK(vkBeginCommandBuffer(cb,&begin)); vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline); vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,2,sets.data(),0,nullptr);
 const std::array<uint32_t,2> firstPush={1664525u,1013904223u}; const std::array<uint32_t,2> secondPush={0x6a09e667u,7u}; vkCmdPushConstants(cb,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,8,firstPush.data()); vkCmdPushConstants(cb,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,8,8,secondPush.data());
 vkCmdDispatch(cb,kCount/kLocalSize,1,1); VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT; vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr); VK_CHECK(vkEndCommandBuffer(cb));
 VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; VK_CHECK(vkCreateFence(device,&fi,nullptr,&fence)); VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cb; VK_CHECK(vkQueueSubmit(queue,1,&submit,fence)); VK_CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,5ull*1000000000ull));
 void* mapped=nullptr; VK_CHECK(vkMapMemory(device,buffers[1].memory,0,VK_WHOLE_SIZE,0,&mapped)); if(!buffers[1].coherent) { VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory=buffers[1].memory; range.offset=0; range.size=VK_WHOLE_SIZE; VK_CHECK(vkInvalidateMappedMemoryRanges(device,1,&range)); }
 auto* output=static_cast<uint32_t*>(mapped); uint32_t mismatch=kCount; for(uint32_t i=0;i<kCount;++i) if(output[i]!=expectedValue(i)) { mismatch=i; break; } uint32_t actualChecksum=checksum(output); uint32_t actual=mismatch<kCount?output[mismatch]:0, expected=mismatch<kCount?expectedValue(mismatch):0; vkUnmapMemory(device,buffers[1].memory);
 vkDestroyFence(device,fence,nullptr); vkFreeCommandBuffers(device,commandPool,1,&cb); vkDestroyCommandPool(device,commandPool,nullptr); vkDestroyPipeline(device,pipeline,nullptr); vkDestroyPipelineLayout(device,pipelineLayout,nullptr); vkDestroyShaderModule(device,shader,nullptr); vkDestroyDescriptorPool(device,pool,nullptr); for(auto layout:layouts) vkDestroyDescriptorSetLayout(device,layout,nullptr); for(auto& item:buffers) { vkDestroyBuffer(device,item.buffer,nullptr); vkFreeMemory(device,item.memory,nullptr); } vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr);
 if(mismatch<kCount) { fprintf(stderr,"FAIL: element %u expected 0x%08x, got 0x%08x\n",mismatch,expected,actual); return 1; } if(actualChecksum!=kExpectedChecksum) { fprintf(stderr,"FAIL: checksum expected %08x, got %08x\n",kExpectedChecksum,actualChecksum); return 1; }
 printf("PASS: push-descriptor-compute checksum=%08x elements=%u\n",actualChecksum,kCount); return 0;
}
