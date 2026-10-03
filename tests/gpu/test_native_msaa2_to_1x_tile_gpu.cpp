// Exact production 72-byte MSAA2-to-1x physical utility shader proof; no renderer integration.
namespace me::native::shaders {
const unsigned int me_edram_depth_msaa2_to_depth_1x_fs[] = {
#include "me_edram_depth_msaa2_to_depth_1x.inc"
};
}
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
#include <cstddef>
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
static VkShaderModule ProductionModule(VkDevice device,const std::string& path) {
  using namespace me::native::shaders;
  std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("missing production SPV");
  const auto bytes=file.tellg();if(bytes!=std::streamoff(sizeof(me_edram_depth_msaa2_to_depth_1x_fs)))throw std::runtime_error("production inc/file size mismatch");
  std::vector<uint32_t> words(size_t(bytes)/4);file.seekg(0);file.read(reinterpret_cast<char*>(words.data()),bytes);
  if(!file||std::memcmp(words.data(),me_edram_depth_msaa2_to_depth_1x_fs,sizeof(me_edram_depth_msaa2_to_depth_1x_fs)))throw std::runtime_error("production inc/file bytes mismatch");
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.codeSize=sizeof(me_edram_depth_msaa2_to_depth_1x_fs);info.pCode=me_edram_depth_msaa2_to_depth_1x_fs;
  VkShaderModule result{};Check(vkCreateShaderModule(device,&info,nullptr,&result),"exact production module");
  std::printf("productionSpvBytes=%zu exactEmbeddedBytes=true pushABI=72 depthBinding=0 stencilBinding=1 guestFormatMode=%s\n",sizeof(me_edram_depth_msaa2_to_depth_1x_fs),"equal-only");
  return result;
}
int main(int argc,char** argv) try {
  if(argc<2||argc>3)throw std::runtime_error("usage: production-tile-proof <SPIRV dir> [unorm|unhalf|bad-flags mode]");
  uint32_t negative=0, guest_format=257;
  if(argc==3){const std::string mode=argv[2];
    const std::array<const char*,13> bad{{"swap","collapse","skip","unknown-format","mixed-format","half-mismatch","grid","collapsed","64bpp","bad-mask","zero-pitch","dim-mismatch","wrong-tile"}};
    if(mode=="unorm")guest_format=0;else if(mode=="unhalf")guest_format=1;
    else {for(uint32_t i=0;i<bad.size();++i)if(mode==bad[i])negative=i+1;
      if(!negative)throw std::runtime_error("unknown mode");}
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="production-me-msaa2-tile";app.apiVersion=VK_API_VERSION_1_2;
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
  constexpr uint32_t sw=160,dw=240,source_height=16,destination_height=32,sn=sw*source_height*2,dn=dw*destination_height,total=sn+dn;
  struct Push {std::array<uint32_t,4> dimensions{},tiles{2,3,1,3},transfer{2,0,0,0},reserved{};};
  static_assert(sizeof(Push)==64);
  struct ProductionPush {
    std::array<uint32_t,2> source_size{sw,source_height},destination_size{dw,destination_height};
    uint32_t source_pitch=2,destination_pitch=3,source_start=1,destination_start=3,count=2;
    uint32_t source_format=257,destination_format=257,source64=0,destination64=0;
    uint32_t source_x=0,source_y=1,destination_x=0,destination_y=0,stencil_mask=0;
  };
  static_assert(sizeof(ProductionPush)==72);
  static_assert(offsetof(ProductionPush,stencil_mask)==68);
  struct Image {VkImage image{};VkDeviceMemory memory{};VkImageView attachment{},depth{},stencil{};VkFramebuffer init{},load{};uint32_t width=0,height=0,samples=0;};
  std::array<Image,2> images{};images[0].width=sw;images[0].height=source_height;images[0].samples=2;images[1].width=dw;images[1].height=destination_height;images[1].samples=1;
  for(auto& im:images){
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_D32_SFLOAT_S8_UINT;ii.extent={im.width,im.height,1};ii.mipLevels=ii.arrayLayers=1;ii.samples=VkSampleCountFlagBits(im.samples);ii.tiling=VK_IMAGE_TILING_OPTIMAL;ii.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    Check(vkCreateImage(device,&ii,nullptr,&im.image),"image");VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,im.image,&req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=MemoryType(gpu,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(device,&ai,nullptr,&im.memory),"image memory");Check(vkBindImageMemory(device,im.image,im.memory,0),"image bind");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=im.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ii.format;vi.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
    Check(vkCreateImageView(device,&vi,nullptr,&im.attachment),"attachment");vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;Check(vkCreateImageView(device,&vi,nullptr,&im.depth),"depth view");vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;Check(vkCreateImageView(device,&vi,nullptr,&im.stencil),"stencil view");
  }
  struct Buffer {VkBuffer buffer{};VkDeviceMemory memory{};Record* data{};};
  std::array<Buffer,2> buffers{};
  for(auto& b:buffers){
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=total*sizeof(Record);bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    Check(vkCreateBuffer(device,&bi,nullptr,&b.buffer),"buffer");VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,b.buffer,&req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=MemoryType(gpu,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(device,&ai,nullptr,&b.memory),"buffer memory");Check(vkBindBufferMemory(device,b.buffer,b.memory,0),"buffer bind");void* p{};Check(vkMapMemory(device,b.memory,0,VK_WHOLE_SIZE,0,&p),"map");b.data=static_cast<Record*>(p);
  }
  for(uint32_t im=0;im<2;++im)for(uint32_t y=0;y<images[im].height;++y)for(uint32_t x=0;x<images[im].width;++x)for(uint32_t s=0;s<images[im].samples;++s){
    const uint32_t i=(im?sn:0)+(y*images[im].width+x)*images[im].samples+s;
    buffers[1].data[i]={0x3E800000u+((x*127+y*911+s*100003+im*170011)&0x1FFFFFu),(x*13+y*31+s*71+im*47)&255u};
  }
  auto poison=[&]{for(uint32_t i=0;i<total;++i)buffers[0].data[i]={0x7FCABCDEu,0xDEADBEEFu};};poison();
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler sampler{};Check(vkCreateSampler(device,&si,nullptr,&sampler),"sampler");
  const auto stages_mask=VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT;
  std::array<VkDescriptorSetLayoutBinding,6> bindings{{{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,stages_mask,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,stages_mask,nullptr},{3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{4,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{5,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
  VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sl.bindingCount=6;sl.pBindings=bindings.data();VkDescriptorSetLayout layout{};Check(vkCreateDescriptorSetLayout(device,&sl,nullptr,&layout),"layout");
  VkPushConstantRange push{stages_mask,0,sizeof(ProductionPush)};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&layout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;VkPipelineLayout pipeline_layout{};Check(vkCreatePipelineLayout(device,&pl,nullptr,&pipeline_layout),"pipeline layout");
  std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,10},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4}}};VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pc.maxSets=3;pc.poolSizeCount=2;pc.pPoolSizes=sizes.data();VkDescriptorPool pool{};Check(vkCreateDescriptorPool(device,&pc,nullptr,&pool),"pool");
  std::array<VkDescriptorSet,2> sets{};std::array<VkDescriptorSetLayout,2> layouts{layout,layout};VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=pool;da.descriptorSetCount=2;da.pSetLayouts=layouts.data();Check(vkAllocateDescriptorSets(device,&da,sets.data()),"sets");
  for(uint32_t im=0;im<2;++im){
    VkDescriptorImageInfo depth{sampler,images[0].depth,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},stencil{sampler,images[0].stencil,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::array<VkDescriptorBufferInfo,2> info{{{buffers[0].buffer,0,total*sizeof(Record)},{buffers[1].buffer,0,total*sizeof(Record)}}};
    std::array<VkWriteDescriptorSet,6> w{};for(uint32_t k=0;k<6;++k){w[k].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w[k].dstSet=sets[im];w[k].dstBinding=k;w[k].descriptorCount=1;}
    w[0].descriptorType=w[2].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&depth;w[2].pImageInfo=&stencil;w[1].descriptorType=w[3].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[1].pBufferInfo=&info[0];w[3].pBufferInfo=&info[1];VkDescriptorImageInfo depth1{sampler,images[1].depth,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},stencil1{sampler,images[1].stencil,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};w[4].descriptorType=w[5].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[4].pImageInfo=&depth1;w[5].pImageInfo=&stencil1;vkUpdateDescriptorSets(device,6,w.data(),0,nullptr);
  }
  std::array<VkDescriptorSetLayoutBinding,2> production_bindings{{
    {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
    {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}}};
  VkDescriptorSetLayoutCreateInfo production_sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  production_sl.bindingCount=2;production_sl.pBindings=production_bindings.data();
  VkDescriptorSetLayout production_layout{};Check(vkCreateDescriptorSetLayout(device,&production_sl,nullptr,&production_layout),"production layout");
  VkPushConstantRange production_push{VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(ProductionPush)};
  VkPipelineLayoutCreateInfo production_pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  production_pl.setLayoutCount=1;production_pl.pSetLayouts=&production_layout;production_pl.pushConstantRangeCount=1;production_pl.pPushConstantRanges=&production_push;
  VkPipelineLayout production_pipeline_layout{};Check(vkCreatePipelineLayout(device,&production_pl,nullptr,&production_pipeline_layout),"production pipeline layout");
  da.descriptorSetCount=1;da.pSetLayouts=&production_layout;VkDescriptorSet production_set{};Check(vkAllocateDescriptorSets(device,&da,&production_set),"production set");
  const std::array<VkDescriptorImageInfo,2> production_images{{
    {sampler,images[0].depth,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    {sampler,images[0].stencil,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
  std::array<VkWriteDescriptorSet,2> production_writes{};for(uint32_t i=0;i<2;++i){auto& w=production_writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=production_set;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&production_images[i];}
  vkUpdateDescriptorSets(device,2,production_writes.data(),0,nullptr);
  std::array<VkRenderPass,3> passes{};
  for(uint32_t kind=0;kind<3;++kind){const bool load=kind==2;
    VkAttachmentDescription ad{};ad.format=VK_FORMAT_D32_SFLOAT_S8_UINT;ad.samples=kind==0?VK_SAMPLE_COUNT_2_BIT:VK_SAMPLE_COUNT_1_BIT;ad.loadOp=ad.stencilLoadOp=load?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_CLEAR;ad.storeOp=ad.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;ad.initialLayout=load?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;ad.finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ar{0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.pDepthStencilAttachment=&ar;
    std::array<VkSubpassDependency,2> deps{};
    deps[0]={VK_SUBPASS_EXTERNAL,0,load?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,load?VK_ACCESS_SHADER_READ_BIT:0u,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,0};
    deps[1]={0,VK_SUBPASS_EXTERNAL,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,0};
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&ad;rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=2;rp.pDependencies=deps.data();Check(vkCreateRenderPass(device,&rp,nullptr,&passes[kind]),"pass");
    auto& im=images[kind==0?0:1];VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=passes[kind];fb.attachmentCount=1;fb.pAttachments=&im.attachment;fb.width=im.width;fb.height=im.height;fb.layers=1;Check(vkCreateFramebuffer(device,&fb,nullptr,load?&im.load:&im.init),"framebuffer");
  }
  const std::string dir=argv[1];std::array<VkShaderModule,4> modules{Module(device,dir+"/tile.vert.spv"),Module(device,dir+"/init.frag.spv"),negative>=1&&negative<=3?Module(device,dir+"/negative-"+std::string(argv[2])+".frag.spv"):ProductionModule(device,dir+"/transfer.frag.spv"),Module(device,dir+"/extract.comp.spv")};
  VkComputePipelineCreateInfo cc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cc.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,modules[3],"main",nullptr};cc.layout=pipeline_layout;VkPipeline compute{};Check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cc,nullptr,&compute),"compute");
  std::array<std::array<VkPipeline,9>,3> graphics{};
  for(uint32_t kind=0;kind<3;++kind)for(uint32_t bit=0;bit<9;++bit){
    std::array<VkPipelineShaderStageCreateInfo,2> st{{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,modules[0],"main",nullptr},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,modules[kind==2?2:1],"main",nullptr}}};
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=kind==0?VK_SAMPLE_COUNT_2_BIT:VK_SAMPLE_COUNT_1_BIT;ms.sampleShadingEnable=kind==0?VK_TRUE:VK_FALSE;ms.minSampleShading=kind==0?1.0f:0.0f;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};ds.depthTestEnable=VK_TRUE;ds.depthWriteEnable=bit==0;ds.depthCompareOp=VK_COMPARE_OP_ALWAYS;ds.stencilTestEnable=VK_TRUE;ds.front={VK_STENCIL_OP_KEEP,VK_STENCIL_OP_REPLACE,VK_STENCIL_OP_KEEP,VK_COMPARE_OP_ALWAYS,255,bit?(1u<<(bit-1)):255u,bit?255u:0u};ds.back=ds.front;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};std::array<VkDynamicState,2> dyn{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=dyn.data();
    VkGraphicsPipelineCreateInfo gc{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gc.stageCount=2;gc.pStages=st.data();gc.pVertexInputState=&vertex;gc.pInputAssemblyState=&ia;gc.pViewportState=&vp;gc.pRasterizationState=&raster;gc.pMultisampleState=&ms;gc.pDepthStencilState=&ds;gc.pColorBlendState=&blend;gc.pDynamicState=&dynamic;gc.layout=kind==2?production_pipeline_layout:pipeline_layout;gc.renderPass=passes[kind];
    Check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gc,nullptr,&graphics[kind][bit]),"graphics");
  }
  VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=family;cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VkCommandPool command_pool{};Check(vkCreateCommandPool(device,&cp,nullptr,&command_pool),"command pool");VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=command_pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cmd{};Check(vkAllocateCommandBuffers(device,&ca,&cmd),"command");
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence{};Check(vkCreateFence(device,&fi,nullptr,&fence),"fence");
  auto begin=[&]{Check(vkResetCommandBuffer(cmd,0),"reset");VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};Check(vkBeginCommandBuffer(cmd,&b),"begin");
    std::array<VkBufferMemoryBarrier,2> barriers{};for(uint32_t k=0;k<2;++k){auto& b=barriers[k];b.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;b.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;b.dstAccessMask=k?VK_ACCESS_SHADER_READ_BIT:VK_ACCESS_SHADER_WRITE_BIT;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.buffer=buffers[k].buffer;b.size=VK_WHOLE_SIZE;}
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,2,barriers.data(),0,nullptr);
  };
  auto finish=[&]{VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.buffer=buffers[0].buffer;b.size=VK_WHOLE_SIZE;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&b,0,nullptr);Check(vkEndCommandBuffer(cmd),"end");Check(vkResetFences(device,1,&fence),"reset fence");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;Check(vkQueueSubmit(queue,1,&submit,fence),"submit");Check(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull),"successful completed fence");};
  auto draw=[&](uint32_t im,bool load){const uint32_t kind=load?2:im;Push p{};p.dimensions={images[im].width,images[im].height,im?sn:0,im?sn:0};p.transfer[3]=images[im].samples;
    VkRect2D whole{{0,0},{images[im].width,images[im].height}},scissor=load?VkRect2D{{0,16},{120,8}}:whole;
    VkClearValue clear{};clear.depthStencil={1,0};VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rb.renderPass=passes[kind];rb.framebuffer=load?images[im].load:images[im].init;rb.renderArea=whole;rb.clearValueCount=1;rb.pClearValues=&clear;vkCmdBeginRenderPass(cmd,&rb,VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0,0,float(images[im].width),float(images[im].height),0,1};vkCmdSetViewport(cmd,0,1,&vp);vkCmdSetScissor(cmd,0,1,&scissor);
    const auto set=load?production_set:sets[im];vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,load?production_pipeline_layout:pipeline_layout,0,1,&set,0,nullptr);
    ProductionPush production{};production.source_format=production.destination_format=guest_format;
    if(negative==4)production.source_format|=512u;
    if(negative==5)production.destination_format=0;
    if(negative==6)production.destination_format=1;
    if(negative==7)production.source_x=1;
    if(negative==8)production.source_y=0;
    if(negative==9)production.source64=1;
    if(negative==11)production.source_pitch=0;
    if(negative==12)--production.source_size[0];
    if(negative==13)production.source_start=0;
    for(uint32_t bit=0;bit<9;++bit){
      p.transfer[1]=bit?(1u<<(bit-1)):0;production.stencil_mask=negative==10?256u:p.transfer[1];
      vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,graphics[kind][bit]);
      if(load)vkCmdPushConstants(cmd,production_pipeline_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(production),&production);
      else vkCmdPushConstants(cmd,pipeline_layout,stages_mask,0,sizeof(p),&p);
      vkCmdDraw(cmd,3,1,0,0);
    }vkCmdEndRenderPass(cmd);
  };
  auto extract=[&]{vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,compute);for(uint32_t im=0;im<2;++im){Push p{};p.dimensions={images[im].width,images[im].height,im?sn:0,0};p.transfer[3]=images[im].samples;vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&sets[im],0,nullptr);vkCmdPushConstants(cmd,pipeline_layout,stages_mask,0,sizeof(p),&p);vkCmdDispatch(cmd,(images[im].width+7)/8,(images[im].height+7)/8,1);}};
  begin();draw(0,false);draw(1,false);extract();finish();
  for(uint32_t i=0;i<total;++i)if(!(buffers[0].data[i]==buffers[1].data[i])){std::fprintf(stderr,"INIT mismatch index=%u actual=%08X/%u expected=%08X/%u\n",i,buffers[0].data[i].depth,buffers[0].data[i].stencil,buffers[1].data[i].depth,buffers[1].data[i].stencil);throw std::runtime_error("independent per-pixel sample initialization failed");}
  std::printf("device=%s initVerifiedSamples=%u sampleRate=true standardSamples=true D32S8_2x=true\n",props.deviceName,total);
  std::vector<Record> before(buffers[0].data,buffers[0].data+total);poison();begin();draw(1,true);extract();finish();
  uint32_t changed=0,outside=0,depth_bad=0,stencil_bad=0,source_bad=0,stale=0,outside_bad=0,covered_unchanged=0;
  for(uint32_t i=0;i<sn;++i){source_bad+=!(buffers[0].data[i]==before[i]);stale+=buffers[0].data[i].depth==0x7FCABCDEu||buffers[0].data[i].stencil==0xDEADBEEFu;}
  for(uint32_t y=0;y<destination_height;++y)for(uint32_t x=0;x<dw;++x){
    const uint32_t i=sn+y*dw+x;Record expected=before[i];const uint32_t physical_y=y,tile=(physical_y/16)*3+x/80;
    const bool covered=x<120&&y>=16&&y<24&&tile>=3&&tile<5;
    if(covered){const uint32_t src_tile=1+tile-3,sx=(src_tile%2)*80+x%80,sy=((src_tile/2)*16+physical_y%16)/2,ss=(physical_y&1)^1;expected=before[(sy*sw+sx)*2+ss];++changed;}else ++outside;
    const auto actual=buffers[0].data[i];depth_bad+=actual.depth!=expected.depth;stencil_bad+=actual.stencil!=expected.stencil;outside_bad+=!covered&&!(actual==expected);covered_unchanged+=covered&&(actual==before[i]);stale+=actual.depth==0x7FCABCDEu||actual.stencil==0xDEADBEEFu;
  }
  const bool mismatch=depth_bad||stencil_bad||source_bad||stale;
  std::printf("tile-map srcPitch=2 dstPitch=3 srcStart=1 dstStart=3 tiles=2 scissor=0,16+120x8 changedSamples=%u outsideSamples=%u guest0=host1 guest1=host0\n",changed,outside);
  std::printf("RESULT negative=%u format=%u D32BitMismatches=%u S8Mismatches=%u sourceMismatches=%u outsideMismatches=%u staleObservers=%u fenceComplete=true\n",negative,guest_format,depth_bad,stencil_bad,source_bad,outside_bad,stale);
  std::printf("guardRejectedROIUnchangedSamples=%u / %u\n",covered_unchanged,changed);
  if(negative>=4&&negative!=13&&covered_unchanged!=changed)throw std::runtime_error("invalid-ABI guard modified destination samples");
  Check(vkDeviceWaitIdle(device),"cleanup idle");vkDestroyFence(device,fence,nullptr);vkDestroyCommandPool(device,command_pool,nullptr);
  for(auto& kind:graphics)for(auto p:kind)vkDestroyPipeline(device,p,nullptr);vkDestroyPipeline(device,compute,nullptr);for(auto m:modules)vkDestroyShaderModule(device,m,nullptr);
  for(auto& im:images){vkDestroyFramebuffer(device,im.init,nullptr);vkDestroyFramebuffer(device,im.load,nullptr);vkDestroyImageView(device,im.attachment,nullptr);vkDestroyImageView(device,im.depth,nullptr);vkDestroyImageView(device,im.stencil,nullptr);vkDestroyImage(device,im.image,nullptr);vkFreeMemory(device,im.memory,nullptr);}
  for(auto p:passes)vkDestroyRenderPass(device,p,nullptr);vkDestroyDescriptorPool(device,pool,nullptr);vkDestroyPipelineLayout(device,pipeline_layout,nullptr);vkDestroyPipelineLayout(device,production_pipeline_layout,nullptr);vkDestroyDescriptorSetLayout(device,layout,nullptr);vkDestroyDescriptorSetLayout(device,production_layout,nullptr);vkDestroySampler(device,sampler,nullptr);
  for(auto& b:buffers){vkUnmapMemory(device,b.memory);vkDestroyBuffer(device,b.buffer,nullptr);vkFreeMemory(device,b.memory,nullptr);}vkDestroyDevice(device,nullptr);destroy_debug(instance,messenger,nullptr);vkDestroyInstance(instance,nullptr);
  std::printf("validationErrors=%u explicitLayer=true debugCallback=true cleanupComplete=true\n",validation_errors.load());
  if(validation_errors.load())throw std::runtime_error("validation errors");
  if(mismatch)throw std::runtime_error("exact tile transfer comparison failed");
  if(negative)throw std::runtime_error("negative control unexpectedly passed");
  std::puts("PASS physical2x-to-1x production72 D32/S8 exact partial tile transport and outside LOAD preservation");
  return 0;
} catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
