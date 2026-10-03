
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
#include <bit>
int main(int argc,char** argv) try {
 if(argc!=4)throw std::runtime_error("usage: GPU VERTSPV FRAGSPV EXPECTED_MODE");
 const unsigned mode=unsigned(std::stoul(argv[3]));
 if(mode<1||mode>3)throw std::runtime_error("unsupported mode");
 RequireValidationLayer();
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_2;
 const char* layer="VK_LAYER_KHRONOS_validation";
 const std::array<const char*,2> extensions{VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
 VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
 debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
 debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;debug.pfnUserCallback=ValidationMessage;
 VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;ic.flags=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;ic.enabledExtensionCount=2;ic.ppEnabledExtensionNames=extensions.data();ic.enabledLayerCount=1;ic.ppEnabledLayerNames=&layer;ic.pNext=&debug;
 VkInstance instance{};Check(vkCreateInstance(&ic,nullptr,&instance),"instance");
 auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
 auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));
 if(!create||!destroy)throw std::runtime_error("debug callbacks unavailable");
 VkDebugUtilsMessengerEXT messenger{};Check(create(instance,&debug,nullptr,&messenger),"messenger");
 uint32_t count=0;Check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"devices");
 if(!count)throw std::runtime_error("no devices");std::vector<VkPhysicalDevice> gpus(count);Check(vkEnumeratePhysicalDevices(instance,&count,gpus.data()),"devices");
 VkPhysicalDevice gpu=gpus[0];VkPhysicalDeviceFeatures features{};vkGetPhysicalDeviceFeatures(gpu,&features);
 if(!features.fragmentStoresAndAtomics)throw std::runtime_error("fragment stores unsupported");
 VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(gpu,&props);
 uint32_t nf=0;vkGetPhysicalDeviceQueueFamilyProperties(gpu,&nf,nullptr);std::vector<VkQueueFamilyProperties> qs(nf);vkGetPhysicalDeviceQueueFamilyProperties(gpu,&nf,qs.data());
 uint32_t family=UINT32_MAX;for(uint32_t i=0;i<nf;++i)if(qs[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){family=i;break;}
 if(family==UINT32_MAX)throw std::runtime_error("no graphics queue");
 float priority=1;VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qc.queueFamilyIndex=family;qc.queueCount=1;qc.pQueuePriorities=&priority;
 uint32_t ne=0;Check(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&ne,nullptr),"extensions");std::vector<VkExtensionProperties> es(ne);Check(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&ne,es.data()),"extensions");
 std::vector<const char*> dex;for(const auto& e:es)if(std::strcmp(e.extensionName,"VK_KHR_portability_subset")==0)dex.push_back("VK_KHR_portability_subset");
 VkPhysicalDeviceFeatures enabled{};enabled.fragmentStoresAndAtomics=VK_TRUE;
 VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;dc.pEnabledFeatures=&enabled;dc.enabledExtensionCount=uint32_t(dex.size());dc.ppEnabledExtensionNames=dex.data();
 VkDevice device{};Check(vkCreateDevice(gpu,&dc,nullptr,&device),"device");VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
 VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=64*16;bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
 VkBuffer buffer{};Check(vkCreateBuffer(device,&bi,nullptr,&buffer),"buffer");VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,buffer,&req);
 VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=MemoryType(gpu,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
 VkDeviceMemory memory{};Check(vkAllocateMemory(device,&ai,nullptr,&memory),"memory");Check(vkBindBufferMemory(device,buffer,memory,0),"bind");
 void* mapped{};Check(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,&mapped),"map");auto* words=static_cast<uint32_t*>(mapped);for(unsigned i=0;i<256;++i)words[i]=0xDEADBEEF;
 VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
 VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sl.bindingCount=1;sl.pBindings=&binding;
 VkDescriptorSetLayout layout{};Check(vkCreateDescriptorSetLayout(device,&sl,nullptr,&layout),"layout");
 VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&layout;VkPipelineLayout pipeline_layout{};Check(vkCreatePipelineLayout(device,&pl,nullptr,&pipeline_layout),"pipeline layout");
 VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=1;dp.poolSizeCount=1;dp.pPoolSizes=&size;
 VkDescriptorPool pool{};Check(vkCreateDescriptorPool(device,&dp,nullptr,&pool),"pool");
 VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ds.descriptorPool=pool;ds.descriptorSetCount=1;ds.pSetLayouts=&layout;VkDescriptorSet set{};Check(vkAllocateDescriptorSets(device,&ds,&set),"set");
 VkDescriptorBufferInfo db{buffer,0,bi.size};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&db;vkUpdateDescriptorSets(device,1,&write,0,nullptr);
 VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
 VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.subpassCount=1;rp.pSubpasses=&sub;VkRenderPass render_pass{};Check(vkCreateRenderPass(device,&rp,nullptr,&render_pass),"renderpass");
 VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=render_pass;fb.width=fb.height=8;fb.layers=1;VkFramebuffer framebuffer{};Check(vkCreateFramebuffer(device,&fb,nullptr,&framebuffer),"framebuffer");
 VkShaderModule vs=Module(device,argv[1]),fs=Module(device,argv[2]);
 std::array<VkPipelineShaderStageCreateInfo,2> stages{{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vs,"main",nullptr},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,fs,"main",nullptr}}};
 VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
 VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
 VkViewport viewport{0,0,8,8,0,1};VkRect2D area{{0,0},{8,8}};
 VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&area;
 VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.lineWidth=1;
 VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
 VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
 VkGraphicsPipelineCreateInfo gc{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gc.stageCount=2;gc.pStages=stages.data();gc.pVertexInputState=&vertex;gc.pInputAssemblyState=&assembly;gc.pViewportState=&vp;gc.pRasterizationState=&raster;gc.pMultisampleState=&ms;gc.pColorBlendState=&blend;gc.layout=pipeline_layout;gc.renderPass=render_pass;
 VkPipeline pipeline{};Check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gc,nullptr,&pipeline),"pipeline");
 VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=family;VkCommandPool command_pool{};Check(vkCreateCommandPool(device,&cp,nullptr,&command_pool),"commandpool");
 VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=command_pool;ca.commandBufferCount=1;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;VkCommandBuffer cmd{};Check(vkAllocateCommandBuffers(device,&ca,&cmd),"commandbuffer");
 VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};Check(vkBeginCommandBuffer(cmd,&cb),"begin");
 VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};barrier.buffer=buffer;barrier.size=VK_WHOLE_SIZE;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,1,&barrier,0,nullptr);
 VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=render_pass;begin.framebuffer=framebuffer;begin.renderArea=area;
 vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&set,0,nullptr);vkCmdDraw(cmd,64,1,0,0);vkCmdEndRenderPass(cmd);
 barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&barrier,0,nullptr);
 Check(vkEndCommandBuffer(cmd),"end");VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence{};Check(vkCreateFence(device,&fi,nullptr,&fence),"fence");
 VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;Check(vkQueueSubmit(queue,1,&submit,fence),"submit");Check(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull),"fence wait");
 unsigned mismatch=0,stale=0,zw_mismatch=0;
 for(unsigned i=0;i<64;++i){float x=float(i%8)+.5f,y=float(i/8)+.5f;float gx=mode==1?x*.5f:x-.25f,gy=mode==1?y:y-(mode==2?.25f:-.25f);
  if(words[i*4]!=std::bit_cast<uint32_t>(gx)||words[i*4+1]!=std::bit_cast<uint32_t>(gy))++mismatch;
  if(words[i*4+2]!=std::bit_cast<uint32_t>(.375f)||words[i*4+3]!=std::bit_cast<uint32_t>(1.0f))++zw_mismatch;
  for(unsigned c=0;c<4;++c)stale+=words[i*4+c]==0xDEADBEEF;
 }
 Check(vkDeviceWaitIdle(device),"idle");
 vkDestroyFence(device,fence,nullptr);vkDestroyCommandPool(device,command_pool,nullptr);vkDestroyPipeline(device,pipeline,nullptr);vkDestroyShaderModule(device,vs,nullptr);vkDestroyShaderModule(device,fs,nullptr);vkDestroyFramebuffer(device,framebuffer,nullptr);vkDestroyRenderPass(device,render_pass,nullptr);vkDestroyDescriptorPool(device,pool,nullptr);vkDestroyPipelineLayout(device,pipeline_layout,nullptr);vkDestroyDescriptorSetLayout(device,layout,nullptr);vkUnmapMemory(device,memory);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,memory,nullptr);vkDestroyDevice(device,nullptr);
 destroy(instance,messenger,nullptr);vkDestroyInstance(instance,nullptr);
 std::printf("device=%s mode=%u XYmismatches=%u/64 ZWmismatches=%u staleObservers=%u fenceComplete=true validationErrors=%u cleanupComplete=true\n",props.deviceName,mode,mismatch,zw_mismatch,stale,validation_errors.load());
 if(stale||zw_mismatch||validation_errors.load())throw std::runtime_error("observation/validation failed");
 if(mismatch)throw std::runtime_error("inverse XY mismatch");
 std::puts("PASS exact transformed SPIRV inverse raw XY");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
