// Isolated native2x guest sample-select resolve to single-sample R32.
#include <vulkan/vulkan.h>
#include <array>
#include <vector>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <atomic>
static std::atomic<uint32_t> validation_errors{0};
static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
  VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void*) {
  if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    const uint32_t n=++validation_errors;
    if(n<=16)std::fprintf(stderr,"VALIDATION ERROR %s\n",data->pMessage);
  }
  return VK_FALSE;
}
static void Check(VkResult r,const char* where) {if(r!=VK_SUCCESS) throw std::runtime_error(std::string(where)+" VkResult="+std::to_string(r));}
static uint32_t MemoryType(VkPhysicalDevice gpu,uint32_t bits,VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties p{};vkGetPhysicalDeviceMemoryProperties(gpu,&p);
  for(uint32_t i=0;i<p.memoryTypeCount;++i)if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&flags)==flags)return i;
  throw std::runtime_error("required memory type unavailable");
}
static VkShaderModule Module(VkDevice d,const std::string& path) {
  std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("missing SPIRV "+path);
  auto n=f.tellg();if(n<=0||n%4)throw std::runtime_error("invalid SPIRV size");
  std::vector<uint32_t> code(size_t(n)/4);f.seekg(0);f.read(reinterpret_cast<char*>(code.data()),n);
  VkShaderModuleCreateInfo c{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};c.codeSize=size_t(n);c.pCode=code.data();
  VkShaderModule out{};Check(vkCreateShaderModule(d,&c,nullptr,&out),"module");return out;
}
int main(int argc,char** argv) try {
  if(argc<3||argc>4)throw std::runtime_error("usage: resolve-proof <SPIRVdir> <rawSelector0..7> [wrong-host|unsanitized]");
  const unsigned long parsed=std::stoul(argv[2]);if(parsed>7)throw std::runtime_error("RB_COPY_CONTROL sample selector outside3bitfield");
  const uint32_t raw_select=uint32_t(parsed);
  // SDK SanitizeCopySampleSelect(... k2X, true), exactly draw.cpp707..720.
  const uint32_t sanitized=raw_select==1||raw_select==3?1:0;
  uint32_t negative=0;
  if(argc==4){const std::string mode=argv[3];negative=mode=="wrong-host"?1:mode=="unsanitized"?2:99;if(negative==99)throw std::runtime_error("unknown mode");}
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="isolated-me-msaa2-tile";app.apiVersion=VK_API_VERSION_1_2;
  const char* layer="VK_LAYER_KHRONOS_validation";
  uint32_t layer_count=0;Check(vkEnumerateInstanceLayerProperties(&layer_count,nullptr),"layer count");
  std::vector<VkLayerProperties> layers(layer_count);Check(vkEnumerateInstanceLayerProperties(&layer_count,layers.data()),"layers");
  bool found_layer=false;for(const auto& l:layers)found_layer|=std::strcmp(l.layerName,layer)==0;
  if(!found_layer)throw std::runtime_error("required Khronos validation layer unavailable");
  uint32_t ext_count=0;Check(vkEnumerateInstanceExtensionProperties(nullptr,&ext_count,nullptr),"instance extension count");
  std::vector<VkExtensionProperties> instance_extensions(ext_count);Check(vkEnumerateInstanceExtensionProperties(nullptr,&ext_count,instance_extensions.data()),"instance extensions");
  bool found_debug=false,found_portability=false;for(const auto& e:instance_extensions){found_debug|=std::strcmp(e.extensionName,VK_EXT_DEBUG_UTILS_EXTENSION_NAME)==0;found_portability|=std::strcmp(e.extensionName,VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)==0;}
  if(!found_debug||!found_portability)throw std::runtime_error("required debug-utils/portability extension unavailable");
  const std::array<const char*,2> iext{{VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,VK_EXT_DEBUG_UTILS_EXTENSION_NAME}};
  VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;debug.pfnUserCallback=Validation;
  VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;ic.enabledExtensionCount=uint32_t(iext.size());ic.ppEnabledExtensionNames=iext.data();ic.flags=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;ic.enabledLayerCount=1;ic.ppEnabledLayerNames=&layer;ic.pNext=&debug;
  VkInstance instance{};Check(vkCreateInstance(&ic,nullptr,&instance),"instance");
  const auto create_debug=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
  const auto destroy_debug=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));
  if(!create_debug||!destroy_debug)throw std::runtime_error("debug-utils entrypoints unavailable");
  VkDebugUtilsMessengerEXT messenger{};Check(create_debug(instance,&debug,nullptr,&messenger),"debug messenger");
  uint32_t count=0;Check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"enumerate");if(!count)throw std::runtime_error("no device");
  std::vector<VkPhysicalDevice> devices(count);Check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"devices");VkPhysicalDevice gpu=devices[0];
  VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(gpu,&props);VkPhysicalDeviceFeatures features{};vkGetPhysicalDeviceFeatures(gpu,&features);
  VkImageFormatProperties ip{};Check(vkGetPhysicalDeviceImageFormatProperties(gpu,VK_FORMAT_D32_SFLOAT_S8_UINT,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,0,&ip),"D32S8 format");
  if(!(ip.sampleCounts&VK_SAMPLE_COUNT_2_BIT)||!(props.limits.sampledImageStencilSampleCounts&VK_SAMPLE_COUNT_2_BIT)||!props.limits.standardSampleLocations)throw std::runtime_error("2x D32S8/standard locations unsupported");
  uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(gpu,&families,nullptr);std::vector<VkQueueFamilyProperties> qp(families);vkGetPhysicalDeviceQueueFamilyProperties(gpu,&families,qp.data());
  uint32_t family=UINT32_MAX;for(uint32_t i=0;i<families;++i)if((qp[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){family=i;break;}
  if(family==UINT32_MAX)throw std::runtime_error("no graphics+compute queue");
  float priority=1;VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qc.queueFamilyIndex=family;qc.queueCount=1;qc.pQueuePriorities=&priority;
  uint32_t extensions=0;vkEnumerateDeviceExtensionProperties(gpu,nullptr,&extensions,nullptr);std::vector<VkExtensionProperties> ep(extensions);vkEnumerateDeviceExtensionProperties(gpu,nullptr,&extensions,ep.data());
  std::vector<const char*> dex;for(const auto& e:ep)if(std::strcmp(e.extensionName,"VK_KHR_portability_subset")==0)dex.push_back("VK_KHR_portability_subset");
  if(!features.sampleRateShading)throw std::runtime_error("sampleRateShading required");
  VkPhysicalDeviceFeatures enabled{};enabled.sampleRateShading=VK_TRUE;
  VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;dc.pEnabledFeatures=&enabled;dc.enabledExtensionCount=uint32_t(dex.size());dc.ppEnabledExtensionNames=dex.data();
  VkDevice device{};Check(vkCreateDevice(gpu,&dc,nullptr,&device),"device");VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);

  struct Record {uint32_t depth,stencil; bool operator==(const Record&)const=default;};
  constexpr uint32_t sw=160,dw=240,height=16,sn=sw*height*2,dn=dw*height*2,total=sn+dn,observer_count=total+dw*height;
  struct Push {std::array<uint32_t,4> dimensions{},tiles{2,3,1,3},transfer{2,0,0,0},reserved{};};
  static_assert(sizeof(Push)==64);
  struct Image {VkImage image{};VkDeviceMemory memory{};VkImageView attachment{},depth{},stencil{};VkFramebuffer init{},load{};uint32_t width;};
  std::array<Image,2> images{};images[0].width=sw;images[1].width=dw;
  for(auto& im:images){
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_D32_SFLOAT_S8_UINT;ii.extent={im.width,height,1};ii.mipLevels=ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_2_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;ii.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    Check(vkCreateImage(device,&ii,nullptr,&im.image),"image");VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,im.image,&req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=MemoryType(gpu,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(device,&ai,nullptr,&im.memory),"image memory");Check(vkBindImageMemory(device,im.image,im.memory,0),"image bind");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=im.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ii.format;vi.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
    Check(vkCreateImageView(device,&vi,nullptr,&im.attachment),"attachment");vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;Check(vkCreateImageView(device,&vi,nullptr,&im.depth),"depth view");vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;Check(vkCreateImageView(device,&vi,nullptr,&im.stencil),"stencil view");
  }
  struct Buffer {VkBuffer buffer{};VkDeviceMemory memory{};Record* data{};};
  std::array<Buffer,2> buffers{};
  for(auto& b:buffers){
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=observer_count*sizeof(Record);bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    Check(vkCreateBuffer(device,&bi,nullptr,&b.buffer),"buffer");VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,b.buffer,&req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=MemoryType(gpu,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(device,&ai,nullptr,&b.memory),"buffer memory");Check(vkBindBufferMemory(device,b.buffer,b.memory,0),"buffer bind");void* p{};Check(vkMapMemory(device,b.memory,0,VK_WHOLE_SIZE,0,&p),"map");b.data=static_cast<Record*>(p);
  }

  // Capability guards before creating single-sample storage/transfer R32 backing.
  VkImageFormatProperties r32props{};Check(vkGetPhysicalDeviceImageFormatProperties(gpu,VK_FORMAT_R32_SFLOAT,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,0,&r32props),"R32 storage format");
  if(!(r32props.sampleCounts&VK_SAMPLE_COUNT_1_BIT))throw std::runtime_error("R32 single sample unsupported");
  VkImageCreateInfo ri{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ri.imageType=VK_IMAGE_TYPE_2D;ri.format=VK_FORMAT_R32_SFLOAT;ri.extent={dw,height,1};ri.mipLevels=ri.arrayLayers=1;ri.samples=VK_SAMPLE_COUNT_1_BIT;ri.tiling=VK_IMAGE_TILING_OPTIMAL;ri.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VkImage resolved{};VkDeviceMemory resolved_memory{};VkImageView resolved_view{};Check(vkCreateImage(device,&ri,nullptr,&resolved),"R32 image");VkMemoryRequirements rr{};vkGetImageMemoryRequirements(device,resolved,&rr);
  VkMemoryAllocateInfo ra{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ra.allocationSize=rr.size;ra.memoryTypeIndex=MemoryType(gpu,rr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);Check(vkAllocateMemory(device,&ra,nullptr,&resolved_memory),"R32 memory");Check(vkBindImageMemory(device,resolved,resolved_memory,0),"R32 bind");
  VkImageViewCreateInfo rv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};rv.image=resolved;rv.viewType=VK_IMAGE_VIEW_TYPE_2D;rv.format=VK_FORMAT_R32_SFLOAT;rv.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};Check(vkCreateImageView(device,&rv,nullptr,&resolved_view),"R32 view");
  for(uint32_t im=0;im<2;++im)for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<images[im].width;++x)for(uint32_t s=0;s<2;++s){
    const uint32_t i=(im?sn:0)+(y*images[im].width+x)*2+s;
    buffers[1].data[i]={0x3E800000u+((x*127+y*911+s*100003+im*170011)&0x1FFFFFu),(x*13+y*31+s*71+im*47)&255u};
  }
  auto poison=[&]{for(uint32_t i=0;i<observer_count;++i)buffers[0].data[i]={0x7FCABCDEu,0xDEADBEEFu};};poison();
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler sampler{};Check(vkCreateSampler(device,&si,nullptr,&sampler),"sampler");
  const auto stages_mask=VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT;
  std::array<VkDescriptorSetLayoutBinding,5> bindings{{{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,stages_mask,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,stages_mask,nullptr},{3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{4,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
  VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sl.bindingCount=5;sl.pBindings=bindings.data();VkDescriptorSetLayout layout{};Check(vkCreateDescriptorSetLayout(device,&sl,nullptr,&layout),"layout");
  VkPushConstantRange push{stages_mask,0,sizeof(Push)};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&layout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;VkPipelineLayout pipeline_layout{};Check(vkCreatePipelineLayout(device,&pl,nullptr,&pipeline_layout),"pipeline layout");
  // Production-compatible resolve ABI is sampled depth0/storage R32at1.
  // Observer has a separate layout (its binding1 is an SSBO), no shader patch.
  std::array<VkDescriptorSetLayoutBinding,2> resolve_bindings{{{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
  VkDescriptorSetLayoutCreateInfo rsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};rsl.bindingCount=2;rsl.pBindings=resolve_bindings.data();VkDescriptorSetLayout resolve_layout{};Check(vkCreateDescriptorSetLayout(device,&rsl,nullptr,&resolve_layout),"resolve set layout");
  VkPushConstantRange resolve_push{VK_SHADER_STAGE_COMPUTE_BIT,0,48};VkPipelineLayoutCreateInfo rpl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};rpl.setLayoutCount=1;rpl.pSetLayouts=&resolve_layout;rpl.pushConstantRangeCount=1;rpl.pPushConstantRanges=&resolve_push;VkPipelineLayout resolve_pipeline_layout{};Check(vkCreatePipelineLayout(device,&rpl,nullptr,&resolve_pipeline_layout),"resolve pipeline layout");
  std::array<VkDescriptorPoolSize,3> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,5},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,3}}};VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pc.maxSets=3;pc.poolSizeCount=3;pc.pPoolSizes=sizes.data();VkDescriptorPool pool{};Check(vkCreateDescriptorPool(device,&pc,nullptr,&pool),"pool");
  std::array<VkDescriptorSet,2> sets{};std::array<VkDescriptorSetLayout,2> layouts{layout,layout};VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=pool;da.descriptorSetCount=2;da.pSetLayouts=layouts.data();Check(vkAllocateDescriptorSets(device,&da,sets.data()),"sets");
  for(uint32_t im=0;im<2;++im){
    VkDescriptorImageInfo depth{sampler,images[im].depth,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},stencil{sampler,images[im].stencil,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::array<VkDescriptorBufferInfo,2> info{{{buffers[0].buffer,0,observer_count*sizeof(Record)},{buffers[1].buffer,0,observer_count*sizeof(Record)}}};
    VkDescriptorImageInfo resolve_info{VK_NULL_HANDLE,resolved_view,VK_IMAGE_LAYOUT_GENERAL};std::array<VkWriteDescriptorSet,5> w{};for(uint32_t k=0;k<5;++k){w[k].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w[k].dstSet=sets[im];w[k].dstBinding=k;w[k].descriptorCount=1;}
    w[0].descriptorType=w[2].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&depth;w[2].pImageInfo=&stencil;w[1].descriptorType=w[3].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[1].pBufferInfo=&info[0];w[3].pBufferInfo=&info[1];w[4].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;w[4].pImageInfo=&resolve_info;vkUpdateDescriptorSets(device,5,w.data(),0,nullptr);
  }
  VkDescriptorSetAllocateInfo rda{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};rda.descriptorPool=pool;rda.descriptorSetCount=1;rda.pSetLayouts=&resolve_layout;VkDescriptorSet resolve_set{};Check(vkAllocateDescriptorSets(device,&rda,&resolve_set),"resolve descriptor set");
  VkDescriptorImageInfo resolve_source{sampler,images[0].depth,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},resolve_destination{VK_NULL_HANDLE,resolved_view,VK_IMAGE_LAYOUT_GENERAL};
  std::array<VkWriteDescriptorSet,2> resolve_writes{};
  for(uint32_t k=0;k<2;++k){auto& w=resolve_writes[k];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=resolve_set;w.dstBinding=k;w.descriptorCount=1;w.descriptorType=k?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=k?&resolve_destination:&resolve_source;}
  vkUpdateDescriptorSets(device,2,resolve_writes.data(),0,nullptr);
  std::array<VkRenderPass,2> passes{};
  for(uint32_t load=0;load<2;++load){
    VkAttachmentDescription ad{};ad.format=VK_FORMAT_D32_SFLOAT_S8_UINT;ad.samples=VK_SAMPLE_COUNT_2_BIT;ad.loadOp=ad.stencilLoadOp=load?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_CLEAR;ad.storeOp=ad.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;ad.initialLayout=load?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;ad.finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ar{0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.pDepthStencilAttachment=&ar;
    std::array<VkSubpassDependency,2> deps{};
    deps[0]={VK_SUBPASS_EXTERNAL,0,load?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,load?VK_ACCESS_SHADER_READ_BIT:0u,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,0};
    deps[1]={0,VK_SUBPASS_EXTERNAL,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,0};
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&ad;rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=2;rp.pDependencies=deps.data();Check(vkCreateRenderPass(device,&rp,nullptr,&passes[load]),"pass");
    for(auto& im:images){VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=passes[load];fb.attachmentCount=1;fb.pAttachments=&im.attachment;fb.width=im.width;fb.height=height;fb.layers=1;Check(vkCreateFramebuffer(device,&fb,nullptr,load?&im.load:&im.init),"framebuffer");}
  }
  const std::string dir=argv[1];std::array<VkShaderModule,4> modules{Module(device,dir+"/tile.vert.spv"),Module(device,dir+"/init.frag.spv"),Module(device,dir+"/resolve.comp.spv"),Module(device,dir+"/extract.comp.spv")};
  VkComputePipelineCreateInfo cc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cc.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,modules[3],"main",nullptr};cc.layout=pipeline_layout;VkPipeline compute{};Check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cc,nullptr,&compute),"compute");
  VkPipeline resolve_pipeline{},resolve_extract{};cc.stage.module=modules[2];cc.layout=resolve_pipeline_layout;Check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cc,nullptr,&resolve_pipeline),"resolve compute");VkShaderModule resolve_extract_module=Module(device,dir+"/resolve-extract.comp.spv");cc.stage.module=resolve_extract_module;cc.layout=pipeline_layout;Check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cc,nullptr,&resolve_extract),"R32 readback compute");
  std::array<std::array<VkPipeline,9>,1> graphics{};
  for(uint32_t kind=0;kind<1;++kind)for(uint32_t bit=0;bit<9;++bit){
    std::array<VkPipelineShaderStageCreateInfo,2> st{{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,modules[0],"main",nullptr},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,modules[kind+1],"main",nullptr}}};
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_2_BIT;ms.sampleShadingEnable=VK_TRUE;ms.minSampleShading=1;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};ds.depthTestEnable=VK_TRUE;ds.depthWriteEnable=bit==0;ds.depthCompareOp=VK_COMPARE_OP_ALWAYS;ds.stencilTestEnable=VK_TRUE;ds.front={VK_STENCIL_OP_KEEP,VK_STENCIL_OP_REPLACE,VK_STENCIL_OP_KEEP,VK_COMPARE_OP_ALWAYS,255,bit?(1u<<(bit-1)):255u,bit?255u:0u};ds.back=ds.front;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};std::array<VkDynamicState,2> dyn{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=dyn.data();
    VkGraphicsPipelineCreateInfo gc{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gc.stageCount=2;gc.pStages=st.data();gc.pVertexInputState=&vertex;gc.pInputAssemblyState=&ia;gc.pViewportState=&vp;gc.pRasterizationState=&raster;gc.pMultisampleState=&ms;gc.pDepthStencilState=&ds;gc.pColorBlendState=&blend;gc.pDynamicState=&dynamic;gc.layout=pipeline_layout;gc.renderPass=passes[kind];
    Check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gc,nullptr,&graphics[kind][bit]),"graphics");
  }
  VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=family;cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VkCommandPool command_pool{};Check(vkCreateCommandPool(device,&cp,nullptr,&command_pool),"command pool");VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=command_pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cmd{};Check(vkAllocateCommandBuffers(device,&ca,&cmd),"command");
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence{};Check(vkCreateFence(device,&fi,nullptr,&fence),"fence");
  auto begin=[&]{Check(vkResetCommandBuffer(cmd,0),"reset");VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};Check(vkBeginCommandBuffer(cmd,&b),"begin");
    std::array<VkBufferMemoryBarrier,2> barriers{};for(uint32_t k=0;k<2;++k){auto& b=barriers[k];b.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;b.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;b.dstAccessMask=k?VK_ACCESS_SHADER_READ_BIT:VK_ACCESS_SHADER_WRITE_BIT;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.buffer=buffers[k].buffer;b.size=VK_WHOLE_SIZE;}
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,2,barriers.data(),0,nullptr);
  };
  auto finish=[&]{VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.buffer=buffers[0].buffer;b.size=VK_WHOLE_SIZE;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&b,0,nullptr);Check(vkEndCommandBuffer(cmd),"end");Check(vkResetFences(device,1,&fence),"reset fence");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;Check(vkQueueSubmit(queue,1,&submit,fence),"submit");Check(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull),"successful completed fence");};
  auto draw=[&](uint32_t im){Push p{};p.dimensions={images[im].width,height,im?sn:0,im?sn:0};
    VkRect2D whole{{0,0},{images[im].width,height}},scissor=whole;
    VkClearValue clear{};clear.depthStencil={1,0};VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rb.renderPass=passes[0];rb.framebuffer=images[im].init;rb.renderArea=whole;rb.clearValueCount=1;rb.pClearValues=&clear;vkCmdBeginRenderPass(cmd,&rb,VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0,0,float(images[im].width),float(height),0,1};vkCmdSetViewport(cmd,0,1,&vp);vkCmdSetScissor(cmd,0,1,&scissor);
    const auto set=sets[im];vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&set,0,nullptr);
    for(uint32_t bit=0;bit<9;++bit){p.transfer[1]=bit?(1u<<(bit-1)):0;vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,graphics[0][bit]);vkCmdPushConstants(cmd,pipeline_layout,stages_mask,0,sizeof(p),&p);vkCmdDraw(cmd,3,1,0,0);}vkCmdEndRenderPass(cmd);
  };
  auto extract=[&]{vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,compute);for(uint32_t im=0;im<2;++im){Push p{};p.dimensions={images[im].width,height,im?sn:0,0};vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&sets[im],0,nullptr);vkCmdPushConstants(cmd,pipeline_layout,stages_mask,0,sizeof(p),&p);vkCmdDispatch(cmd,(images[im].width+7)/8,(height+7)/8,1);}};

  auto read_r32=[&]{Push p{};p.dimensions={dw,height,total,0};vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,resolve_extract);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&sets[0],0,nullptr);vkCmdPushConstants(cmd,pipeline_layout,stages_mask,0,sizeof(p),&p);vkCmdDispatch(cmd,(dw+7)/8,(height+7)/8,1);};
  auto r32_barrier=[&](VkPipelineStageFlags srcstage,VkAccessFlags srcaccess,VkPipelineStageFlags dststage,VkAccessFlags dstaccess,VkImageLayout oldlayout){
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=srcaccess;b.dstAccessMask=dstaccess;b.oldLayout=oldlayout;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=resolved;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(cmd,srcstage,dststage,0,0,nullptr,0,nullptr,1,&b);
  };
  begin();draw(0);draw(1);
  r32_barrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_IMAGE_LAYOUT_UNDEFINED);
  VkClearColorValue initial{};initial.float32[0]=-.75f;const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(cmd,resolved,VK_IMAGE_LAYOUT_GENERAL,&initial,1,&range);
  r32_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,VK_IMAGE_LAYOUT_GENERAL);
  extract();read_r32();finish();
  for(uint32_t i=0;i<total;++i)if(!(buffers[0].data[i]==buffers[1].data[i])){std::fprintf(stderr,"INIT mismatch index=%u actual=%08X/%u expected=%08X/%u\n",i,buffers[0].data[i].depth,buffers[0].data[i].stencil,buffers[1].data[i].depth,buffers[1].data[i].stencil);throw std::runtime_error("independent per-pixel sample initialization failed");}

  for(uint32_t i=0;i<dw*height;++i)if(buffers[0].data[total+i].depth!=0xBF400000u||buffers[0].data[total+i].stencil!=0xABCD1234u)throw std::runtime_error("initial R32 observer mismatch");
  std::printf("device=%s initVerifiedSamples=%u R32=%u source2x=true destination1x=true\n",props.deviceName,total,dw*height);
  std::vector<Record> before(buffers[0].data,buffers[0].data+observer_count);
  poison();begin();
  // True shifted ROI (source20,3 destination11,4 extent93x7), no average.
  Push p{};p.dimensions={sw,height,dw,height};p.tiles={20,3,11,4};p.transfer={93,7,0x40000000u,sanitized};
  if(negative==1)p.transfer[3]^=1u; // Wrong hostsample after valid normalization.
  if(negative==2)p.transfer[3]=4;   // Deliberately unsanitized k01: shader must reject.
  r32_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_IMAGE_LAYOUT_GENERAL);
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,resolve_pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,resolve_pipeline_layout,0,1,&resolve_set,0,nullptr);vkCmdPushConstants(cmd,resolve_pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,48,&p);vkCmdDispatch(cmd,(93+7)/8,(7+7)/8,1);
  r32_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,VK_IMAGE_LAYOUT_GENERAL);
  extract();read_r32();finish();
  uint32_t mismatch_count=0,source_bad=0,stale=0,changed=0,outside=0;
  for(uint32_t i=0;i<total;++i){source_bad+=!(buffers[0].data[i]==before[i]);stale+=buffers[0].data[i].depth==0x7FCABCDEu||buffers[0].data[i].stencil==0xDEADBEEFu;}
  for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<dw;++x){
    const uint32_t i=total+y*dw+x;uint32_t expected=before[i].depth;
    if(x>=11&&x<104&&y>=4&&y<11){const uint32_t source_index=((y-4+3)*sw+(x-11+20))*2+(sanitized^1u);float depth{};std::memcpy(&depth,&before[source_index].depth,4);depth*=2;std::memcpy(&expected,&depth,4);++changed;}else ++outside;
    const auto actual=buffers[0].data[i];mismatch_count+=actual.depth!=expected||actual.stencil!=0xABCD1234u;stale+=actual.depth==0x7FCABCDEu||actual.stencil==0xDEADBEEFu;
  }
  const bool mismatch=mismatch_count||source_bad||stale;
  std::printf("RESULT raw=%u sanitized=%u SDKguestHost=%u negative=%u R32BitMismatches=%u sourceD32S8Mismatches=%u staleObservers=%u changedPixels=%u outsidePixels=%u fenceComplete=true\n",raw_select,sanitized,sanitized^1u,negative,mismatch_count,source_bad,stale,changed,outside);
  Check(vkDeviceWaitIdle(device),"cleanup idle");vkDestroyFence(device,fence,nullptr);vkDestroyCommandPool(device,command_pool,nullptr);
  for(auto& kind:graphics)for(auto p:kind)vkDestroyPipeline(device,p,nullptr);vkDestroyPipeline(device,compute,nullptr);vkDestroyPipeline(device,resolve_pipeline,nullptr);vkDestroyPipeline(device,resolve_extract,nullptr);vkDestroyShaderModule(device,resolve_extract_module,nullptr);vkDestroyImageView(device,resolved_view,nullptr);vkDestroyImage(device,resolved,nullptr);vkFreeMemory(device,resolved_memory,nullptr);for(auto m:modules)vkDestroyShaderModule(device,m,nullptr);
  for(auto& im:images){vkDestroyFramebuffer(device,im.init,nullptr);vkDestroyFramebuffer(device,im.load,nullptr);vkDestroyImageView(device,im.attachment,nullptr);vkDestroyImageView(device,im.depth,nullptr);vkDestroyImageView(device,im.stencil,nullptr);vkDestroyImage(device,im.image,nullptr);vkFreeMemory(device,im.memory,nullptr);}
  for(auto p:passes)vkDestroyRenderPass(device,p,nullptr);vkDestroyDescriptorPool(device,pool,nullptr);vkDestroyPipelineLayout(device,pipeline_layout,nullptr);vkDestroyPipelineLayout(device,resolve_pipeline_layout,nullptr);vkDestroyDescriptorSetLayout(device,layout,nullptr);vkDestroyDescriptorSetLayout(device,resolve_layout,nullptr);vkDestroySampler(device,sampler,nullptr);
  for(auto& b:buffers){vkUnmapMemory(device,b.memory);vkDestroyBuffer(device,b.buffer,nullptr);vkFreeMemory(device,b.memory,nullptr);}vkDestroyDevice(device,nullptr);destroy_debug(instance,messenger,nullptr);vkDestroyInstance(instance,nullptr);
  std::printf("validationErrors=%u explicitLayer=true debugCallback=true cleanupComplete=true\n",validation_errors.load());
  if(validation_errors.load())throw std::runtime_error("validation errors");
  if(mismatch)throw std::runtime_error("exact MSAA guest resolve comparison failed");
  if(negative)throw std::runtime_error("negative control unexpectedly passed");
  std::puts("PASS native2x sample-select guestspace R32 shifted ROI exact and outside preserved");
  return 0;
} catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
