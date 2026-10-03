// Isolated Vulkan sample-position proof; NOT a game renderer/MSAA acceptance.
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
static std::atomic<unsigned> validation_errors{0};
static VKAPI_ATTR VkBool32 VKAPI_CALL ValidationMessage(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validation_errors;
  std::fprintf(stderr, "validation %s: %s\n",
      severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR" : "WARNING",
      data && data->pMessage ? data->pMessage : "no message");
  return VK_FALSE;
}
static void Check(VkResult r,const char* where) {if(r!=VK_SUCCESS) throw std::runtime_error(std::string(where)+" VkResult="+std::to_string(r));}
static void RequireValidationLayer() {
  uint32_t count=0;
  Check(vkEnumerateInstanceLayerProperties(&count,nullptr),"enumerate layers");
  std::vector<VkLayerProperties> layers(count);
  Check(vkEnumerateInstanceLayerProperties(&count,layers.data()),"layers");
  for (const auto& layer:layers)
    if (std::strcmp(layer.layerName,"VK_LAYER_KHRONOS_validation")==0) return;
  throw std::runtime_error("required VK_LAYER_KHRONOS_validation unavailable");
}
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
  if(argc<2||argc>3)throw std::runtime_error("usage: test_native_msaa_depth_gpu <SPIRV directory> [plain|derivative]");
  const bool plain_only=argc==3&&std::string(argv[2])=="plain";
  const bool derivative=argc==3&&std::string(argv[2])=="derivative";
  if(argc==3&&!plain_only&&!derivative)throw std::runtime_error("unsupported test mode");
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="isolated-me-msaa2-plane";app.apiVersion=VK_API_VERSION_1_2;
  RequireValidationLayer();
  const std::array<const char*,2> iext{{VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,VK_EXT_DEBUG_UTILS_EXTENSION_NAME}};
  const char* layer="VK_LAYER_KHRONOS_validation";
  VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  debug.pfnUserCallback=ValidationMessage;
  VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;ic.enabledExtensionCount=uint32_t(iext.size());ic.ppEnabledExtensionNames=iext.data();ic.flags=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;ic.enabledLayerCount=1;ic.ppEnabledLayerNames=&layer;ic.pNext=&debug;
  VkInstance instance{};Check(vkCreateInstance(&ic,nullptr,&instance),"instance");
  const auto create_debug=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
  const auto destroy_debug=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));
  if (!create_debug||!destroy_debug) throw std::runtime_error("required validation callback functions unavailable");
  VkDebugUtilsMessengerEXT messenger{};Check(create_debug(instance,&debug,nullptr,&messenger),"validation callback");
  std::printf("requiredValidationLayer=VK_LAYER_KHRONOS_validation debugCallbackActive=true VulkanAPI=1.2\n");
  uint32_t count=0;Check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"enumerate");if(!count)throw std::runtime_error("no device");
  std::vector<VkPhysicalDevice> devices(count);Check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"devices");VkPhysicalDevice gpu=devices[0];
  VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(gpu,&props);VkPhysicalDeviceFeatures features{};vkGetPhysicalDeviceFeatures(gpu,&features);
  VkImageFormatProperties ip{};Check(vkGetPhysicalDeviceImageFormatProperties(gpu,VK_FORMAT_D32_SFLOAT_S8_UINT,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,0,&ip),"D32S8 format");
  if(!(ip.sampleCounts&VK_SAMPLE_COUNT_2_BIT)||!props.limits.standardSampleLocations)throw std::runtime_error("2x D32S8/standard locations unsupported");
  uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(gpu,&families,nullptr);std::vector<VkQueueFamilyProperties> qp(families);vkGetPhysicalDeviceQueueFamilyProperties(gpu,&families,qp.data());
  uint32_t family=UINT32_MAX;for(uint32_t i=0;i<families;++i)if((qp[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){family=i;break;}
  if(family==UINT32_MAX)throw std::runtime_error("no graphics+compute queue");
  float priority=1;VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qc.queueFamilyIndex=family;qc.queueCount=1;qc.pQueuePriorities=&priority;
  uint32_t extensions=0;vkEnumerateDeviceExtensionProperties(gpu,nullptr,&extensions,nullptr);std::vector<VkExtensionProperties> ep(extensions);vkEnumerateDeviceExtensionProperties(gpu,nullptr,&extensions,ep.data());
  std::vector<const char*> dex;for(const auto& e:ep)if(std::strcmp(e.extensionName,"VK_KHR_portability_subset")==0)dex.push_back("VK_KHR_portability_subset");
  const bool quant_supported=features.sampleRateShading&&features.fragmentStoresAndAtomics;
  VkPhysicalDeviceFeatures enabled{};enabled.sampleRateShading=features.sampleRateShading;enabled.fragmentStoresAndAtomics=features.fragmentStoresAndAtomics;
  VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;dc.pEnabledFeatures=&enabled;dc.enabledExtensionCount=uint32_t(dex.size());dc.ppEnabledExtensionNames=dex.data();
  VkDevice device{};Check(vkCreateDevice(gpu,&dc,nullptr,&device),"device");VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_D32_SFLOAT_S8_UINT;ii.extent={4,4,1};ii.mipLevels=ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_2_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;ii.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
  VkImage image{};Check(vkCreateImage(device,&ii,nullptr,&image),"image");VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,image,&req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=MemoryType(gpu,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkDeviceMemory imem{};Check(vkAllocateMemory(device,&ai,nullptr,&imem),"image memory");Check(vkBindImageMemory(device,image,imem,0),"image bind");
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ii.format;vi.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
  VkImageView attachment{},sampled{};Check(vkCreateImageView(device,&vi,nullptr,&attachment),"attachment view");vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;Check(vkCreateImageView(device,&vi,nullptr,&sampled),"depth view");
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=sizeof(float)*32+sizeof(uint32_t)*2;bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  VkBuffer buffer{};Check(vkCreateBuffer(device,&bi,nullptr,&buffer),"buffer");vkGetBufferMemoryRequirements(device,buffer,&req);ai.allocationSize=req.size;ai.memoryTypeIndex=MemoryType(gpu,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory bmem{};Check(vkAllocateMemory(device,&ai,nullptr,&bmem),"buffer memory");Check(vkBindBufferMemory(device,buffer,bmem,0),"buffer bind");void* mapped{};Check(vkMapMemory(device,bmem,0,VK_WHOLE_SIZE,0,&mapped),"map");
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler sampler{};Check(vkCreateSampler(device,&si,nullptr,&sampler),"sampler");
  std::array<VkDescriptorSetLayoutBinding,2> bindings{{{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}}};
  VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sl.bindingCount=2;sl.pBindings=bindings.data();VkDescriptorSetLayout layout{};Check(vkCreateDescriptorSetLayout(device,&sl,nullptr,&layout),"set layout");
  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT,0,16};
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&layout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;VkPipelineLayout pipeline_layout{};Check(vkCreatePipelineLayout(device,&pl,nullptr,&pipeline_layout),"pipeline layout");
  std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}}};VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pc.maxSets=1;pc.poolSizeCount=2;pc.pPoolSizes=sizes.data();VkDescriptorPool pool{};Check(vkCreateDescriptorPool(device,&pc,nullptr,&pool),"pool");
  VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=pool;da.descriptorSetCount=1;da.pSetLayouts=&layout;VkDescriptorSet set{};Check(vkAllocateDescriptorSets(device,&da,&set),"descriptor set");
  VkDescriptorImageInfo di{sampler,sampled,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkDescriptorBufferInfo db{buffer,0,bi.size};std::array<VkWriteDescriptorSet,2> writes{};
  for(auto& w:writes){w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=set;w.descriptorCount=1;}writes[0].dstBinding=0;writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[0].pImageInfo=&di;writes[1].dstBinding=1;writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[1].pBufferInfo=&db;vkUpdateDescriptorSets(device,2,writes.data(),0,nullptr);
  VkAttachmentDescription ad{};ad.format=ii.format;ad.samples=ii.samples;ad.loadOp=ad.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;ad.storeOp=ad.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;ad.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;ad.finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkAttachmentReference ar{0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.pDepthStencilAttachment=&ar;
  std::array<VkSubpassDependency,2> deps{};deps[0]={VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,0,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,0};deps[1]={0,VK_SUBPASS_EXTERNAL,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,0};
  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&ad;rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=2;rp.pDependencies=deps.data();VkRenderPass render_pass{};Check(vkCreateRenderPass(device,&rp,nullptr,&render_pass),"render pass");
  VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=render_pass;fb.attachmentCount=1;fb.pAttachments=&attachment;fb.width=fb.height=4;fb.layers=1;VkFramebuffer framebuffer{};Check(vkCreateFramebuffer(device,&fb,nullptr,&framebuffer),"framebuffer");
  const std::string dir=argv[1];VkShaderModule vs=Module(device,dir+"/plane.vert.spv"),fs=Module(device,dir+"/plane.frag.spv"),cs=Module(device,dir+"/extract.comp.spv"),qs{};if(quant_supported)qs=Module(device,dir+(derivative?"/derivative.frag.spv":"/quant.frag.spv"));
  VkComputePipelineCreateInfo cc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cc.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,cs,"main",nullptr};cc.layout=pipeline_layout;VkPipeline compute{};Check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cc,nullptr,&compute),"compute");
  VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=family;cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VkCommandPool command_pool{};Check(vkCreateCommandPool(device,&cp,nullptr,&command_pool),"command pool");
  VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=command_pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cmd{};Check(vkAllocateCommandBuffers(device,&ca,&cmd),"command buffer");
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence{};Check(vkCreateFence(device,&fi,nullptr,&fence),"fence");
  std::printf("device=%s standardSamples=true D32S8_2x=true sampleRate=%u\n",props.deviceName,features.sampleRateShading);
  if(!quant_supported)std::printf("quantization SKIPPED: sample-rate shading / fragment atomics unsupported\n");
  struct Plane {const char* name;std::array<float,4> c;float zmin,zmax;};
  const std::array<Plane,5> planes{{
    {"positive",{.30f,.01f,.02f,0},0,1},
    {"mixed",{.38f,-.009f,.013f,0},0,1},
    {"tie-even-low",{.375f+0x1p-23f,0,0,0},0,1},
    {"tie-even-high",{.375f+0x1.8p-22f,0,0,0},0,1},
    {"viewport",{.30f,.01f,.02f,0},.2f,.8f}}};
  if(derivative&&!quant_supported)throw std::runtime_error("derivative sample-rate trial unsupported");
  const uint32_t plane_count=derivative?uint32_t(planes.size()):1;
  for(uint32_t plane_index=0;plane_index<plane_count;++plane_index)
  for(uint32_t quant=0;quant<((quant_supported&&!plain_only)?2u:1u);++quant) {
    const auto& plane=planes[plane_index];
    std::memset(mapped,0,bi.size);
    std::array<VkPipelineShaderStageCreateInfo,2> stages{{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vs,"main",nullptr},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,quant?qs:fs,"main",nullptr}}};
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,4,4,plane.zmin,plane.zmax};VkRect2D scissor{{0,0},{4,4}};VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_2_BIT;ms.sampleShadingEnable=quant;ms.minSampleShading=1;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};ds.depthTestEnable=ds.depthWriteEnable=VK_TRUE;ds.depthCompareOp=VK_COMPARE_OP_ALWAYS;ds.stencilTestEnable=VK_TRUE;ds.front={VK_STENCIL_OP_KEEP,VK_STENCIL_OP_REPLACE,VK_STENCIL_OP_KEEP,VK_COMPARE_OP_ALWAYS,255,255,0x5A};ds.back=ds.front;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};VkGraphicsPipelineCreateInfo gc{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gc.stageCount=2;gc.pStages=stages.data();gc.pVertexInputState=&vertex;gc.pInputAssemblyState=&ia;gc.pViewportState=&vp;gc.pRasterizationState=&raster;gc.pMultisampleState=&ms;gc.pDepthStencilState=&ds;gc.pColorBlendState=&blend;gc.layout=pipeline_layout;gc.renderPass=render_pass;
    VkPipeline graphics{};Check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gc,nullptr,&graphics),"graphics");
    Check(vkResetCommandBuffer(cmd,0),"reset command");VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};Check(vkBeginCommandBuffer(cmd,&cb),"begin");
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=buffer;host.size=VK_WHOLE_SIZE;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&host,0,nullptr);
    VkClearValue clear{};clear.depthStencil={1,0};VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=render_pass;begin.framebuffer=framebuffer;begin.renderArea=scissor;begin.clearValueCount=1;begin.pClearValues=&clear;vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,graphics);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&set,0,nullptr);vkCmdPushConstants(cmd,pipeline_layout,VK_SHADER_STAGE_VERTEX_BIT,0,16,plane.c.data());vkCmdDraw(cmd,3,1,0,0);vkCmdEndRenderPass(cmd);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,compute);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,nullptr);vkCmdDispatch(cmd,1,1,1);
    host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
    Check(vkEndCommandBuffer(cmd),"end");Check(vkResetFences(device,1,&fence),"reset fence");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;Check(vkQueueSubmit(queue,1,&submit,fence),"submit");Check(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull),"completed fence");
    auto* values=static_cast<const float*>(mapped);double max_error=0,min_separation=1;
    uint32_t quant_mismatches=0,max_code_delta=0;
    for(uint32_t y=0;y<4;++y)for(uint32_t x=0;x<4;++x)for(uint32_t s=0;s<2;++s) {
      float expected=plane.zmin+(plane.zmax-plane.zmin)*(plane.c[0]+plane.c[1]*(float(x)+(s==0?.75f:.25f))+plane.c[2]*(float(y)+(s==0?.75f:.25f)));
      if(quant){float guest=expected*2;if(!(guest>=.5f&&guest<1))throw std::runtime_error("fixture exceeds normalized FLOAT24 profile");uint32_t bits{};std::memcpy(&bits,&guest,4);bits=(bits+3+((bits>>3)&1))&~7u;std::memcpy(&guest,&bits,4);expected=guest*.5f;}
      float actual=values[(y*4+x)*2+s];double error=std::abs(double(actual)-expected);max_error=std::max(max_error,error);
      bool mismatch=!std::isfinite(actual)||(!quant&&error>6e-8);
      if(quant){uint32_t bits{},expected_bits{};std::memcpy(&bits,&actual,4);std::memcpy(&expected_bits,&expected,4);if(bits&7u)throw std::runtime_error("quantized depth contains forbidden mantissa bits");
        const uint32_t code=bits>>3,expected_code=expected_bits>>3;
        const uint32_t delta=code>expected_code?code-expected_code:expected_code-code;
        quant_mismatches+=delta!=0;max_code_delta=std::max(max_code_delta,delta);mismatch|=delta>1;
        if(!plane.c[1]&&!plane.c[2]&&delta)throw std::runtime_error("constant-plane ties-to-even mismatch");}
      if(mismatch){const auto* counters=reinterpret_cast<const uint32_t*>(values+32);std::fprintf(stderr,"mismatch quant=%u xy=%u,%u sample=%u actual=%.9g expected=%.9g sampleInvocations=%u,%u\n",quant,x,y,s,actual,expected,counters[0],counters[1]);throw std::runtime_error("sample plane mismatch");}
      float center=plane.zmin+(plane.zmax-plane.zmin)*(plane.c[0]+plane.c[1]*(float(x)+.5f)+plane.c[2]*(float(y)+.5f));min_separation=std::min(min_separation,std::abs(double(actual)-center));
    }
    const auto* invocations=reinterpret_cast<const uint32_t*>(values+32);if(quant&&(invocations[0]!=16||invocations[1]!=16))throw std::runtime_error("per-sample invocation coverage must be exactly 16/16");
    std::printf("PASS plane=%s quant=%u derivative=%u sample0=(+.25,+.25) sample1=(-.25,-.25) guest0=host1 maxError=%.9g quantCodeMismatches=%u/32 maxCodeDelta=%u centerSeparationMin=%.9g sampleInvocations=%u,%u fenceComplete=true\n",plane.name,quant,derivative,max_error,quant_mismatches,max_code_delta,min_separation,invocations[0],invocations[1]);vkDestroyPipeline(device,graphics,nullptr);
  }
  Check(vkDeviceWaitIdle(device),"idle");
  vkDestroyFence(device,fence,nullptr);vkDestroyCommandPool(device,command_pool,nullptr);vkDestroyPipeline(device,compute,nullptr);vkDestroyShaderModule(device,vs,nullptr);vkDestroyShaderModule(device,fs,nullptr);vkDestroyShaderModule(device,cs,nullptr);if(qs)vkDestroyShaderModule(device,qs,nullptr);vkDestroyFramebuffer(device,framebuffer,nullptr);vkDestroyRenderPass(device,render_pass,nullptr);vkDestroyDescriptorPool(device,pool,nullptr);vkDestroyPipelineLayout(device,pipeline_layout,nullptr);vkDestroyDescriptorSetLayout(device,layout,nullptr);vkDestroySampler(device,sampler,nullptr);vkUnmapMemory(device,bmem);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,bmem,nullptr);vkDestroyImageView(device,sampled,nullptr);vkDestroyImageView(device,attachment,nullptr);vkDestroyImage(device,image,nullptr);vkFreeMemory(device,imem,nullptr);vkDestroyDevice(device,nullptr);
  // Keep the callback active through device-resource teardown, then destroy it
  // explicitly before its parent instance.
  destroy_debug(instance,messenger,nullptr);
  vkDestroyInstance(instance,nullptr);
  if(validation_errors.load())throw std::runtime_error("validation reported "+std::to_string(validation_errors.load())+" errors");
  std::printf("validationErrors=0 explicitRequiredLayer=true\n");
  return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
