/**************************************************************************
 *
 * Copyright 2017 Advanced Micro Devices, Inc.
 * Copyright 2021 Red Hat Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
#include "radv_private.h"

#include "vk_video/vulkan_video_codecs_common.h"
#include "ac_vcn_dec.h"
#include "ac_uvd_dec.h"

#define NUM_H264_REFS 17
#define FB_BUFFER_OFFSET             0x1000
#define FB_BUFFER_SIZE               2048
#define IT_SCALING_TABLE_SIZE        992
#define RDECODE_SESSION_CONTEXT_SIZE (128 * 1024)

/* Not 100% sure this isn't too much but works */
#define VID_DEFAULT_ALIGNMENT 256

static bool
radv_vid_buffer_upload_alloc(struct radv_cmd_buffer *cmd_buffer, unsigned size,
                             unsigned *out_offset, void **ptr)
{
   return radv_cmd_buffer_upload_alloc_aligned(cmd_buffer, size, VID_DEFAULT_ALIGNMENT,
                                               out_offset, ptr);
}

/* generate an stream handle */
static unsigned si_vid_alloc_stream_handle()
{
   static unsigned counter = 0;
   unsigned stream_handle = 0;
   unsigned pid = getpid();
   int i;

   for (i = 0; i < 32; ++i)
      stream_handle |= ((pid >> i) & 1) << (31 - i);

   stream_handle ^= ++counter;
   return stream_handle;
}

void
radv_init_physical_device_decoder(struct radv_physical_device *pdevice)
{
   switch (pdevice->rad_info.family) {
   case CHIP_VEGA10:
   case CHIP_VEGA12:
   case CHIP_VEGA20:
      pdevice->vid_dec_reg.data0 = RUVD_GPCOM_VCPU_DATA0_SOC15;
      pdevice->vid_dec_reg.data1 = RUVD_GPCOM_VCPU_DATA1_SOC15;
      pdevice->vid_dec_reg.cmd = RUVD_GPCOM_VCPU_CMD_SOC15;
      pdevice->vid_dec_reg.cntl = RUVD_ENGINE_CNTL_SOC15;
      break;
   case CHIP_RAVEN:
   case CHIP_RAVEN2:
      pdevice->vid_dec_reg.data0 = RDECODE_VCN1_GPCOM_VCPU_DATA0;
      pdevice->vid_dec_reg.data1 = RDECODE_VCN1_GPCOM_VCPU_DATA1;
      pdevice->vid_dec_reg.cmd = RDECODE_VCN1_GPCOM_VCPU_CMD;
      pdevice->vid_dec_reg.cntl = RDECODE_VCN1_ENGINE_CNTL;
      break;
   case CHIP_NAVI10:
   case CHIP_NAVI12:
   case CHIP_NAVI14:
   case CHIP_RENOIR:
      pdevice->vid_dec_reg.data0 = RDECODE_VCN2_GPCOM_VCPU_DATA0;
      pdevice->vid_dec_reg.data1 = RDECODE_VCN2_GPCOM_VCPU_DATA1;
      pdevice->vid_dec_reg.cmd = RDECODE_VCN2_GPCOM_VCPU_CMD;
      pdevice->vid_dec_reg.cntl = RDECODE_VCN2_ENGINE_CNTL;
      break;
   case CHIP_MI100:
   case CHIP_MI200:
   case CHIP_NAVI21:
   case CHIP_NAVI22:
   case CHIP_NAVI23:
   case CHIP_NAVI24:
   case CHIP_VANGOGH:
   case CHIP_REMBRANDT:
      pdevice->vid_dec_reg.data0 = RDECODE_VCN2_5_GPCOM_VCPU_DATA0;
      pdevice->vid_dec_reg.data1 = RDECODE_VCN2_5_GPCOM_VCPU_DATA1;
      pdevice->vid_dec_reg.cmd = RDECODE_VCN2_5_GPCOM_VCPU_CMD;
      pdevice->vid_dec_reg.cntl = RDECODE_VCN2_5_ENGINE_CNTL;
      break;
   default:
      if (radv_has_uvd(pdevice)) {
         pdevice->vid_dec_reg.data0 = RUVD_GPCOM_VCPU_DATA0;
         pdevice->vid_dec_reg.data1 = RUVD_GPCOM_VCPU_DATA1;
         pdevice->vid_dec_reg.cmd = RUVD_GPCOM_VCPU_CMD;
         pdevice->vid_dec_reg.cntl = RUVD_ENGINE_CNTL;
      }
      break;
   }
}

static bool have_it(struct radv_video_session *vid)
{
   return vid->stream_type == RDECODE_CODEC_H264_PERF || vid->stream_type == RDECODE_CODEC_H265;
}

static unsigned calc_ctx_size_h264_perf(struct radv_video_session *vid)
{
   unsigned width_in_mb, height_in_mb, ctx_size;
   unsigned width = align(vid->vk.max_coded.width, VL_MACROBLOCK_WIDTH);
   unsigned height = align(vid->vk.max_coded.height, VL_MACROBLOCK_HEIGHT);

   unsigned max_references = vid->vk.max_dpb_slots + 1;

   // picture width & height in 16 pixel units
   width_in_mb = width / VL_MACROBLOCK_WIDTH;
   height_in_mb = align(height / VL_MACROBLOCK_HEIGHT, 2);

   ctx_size = max_references * align(width_in_mb * height_in_mb * 192, 256);

   return ctx_size;
}

VkResult
radv_CreateVideoSessionKHR(VkDevice _device,
                           const VkVideoSessionCreateInfoKHR *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkVideoSessionKHR *pVideoSession)
{
   RADV_FROM_HANDLE(radv_device, device, _device);

   struct radv_video_session *vid =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(*vid), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!vid)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   memset(vid, 0, sizeof(struct radv_video_session));

   VkResult result = vk_video_session_init(&device->vk,
                                           &vid->vk,
                                           pCreateInfo);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, pAllocator, vid);
      return result;
   }

   vid->interlaced = false;

   switch (vid->vk.op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
      vid->stream_type = RDECODE_CODEC_H264_PERF;
      break;
   default:
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }

   vid->stream_handle = si_vid_alloc_stream_handle();

   vid->dbg_frame_cnt = 0;
   vid->dpb_type = DPB_MAX_RES;
   vid->db_alignment = (device->physical_device->rad_info.family >= CHIP_RENOIR &&
                        vid->vk.max_coded.width > 32 && 0) ? 64 : 32;

   *pVideoSession = radv_video_session_to_handle(vid);
   return VK_SUCCESS;
}

void
radv_DestroyVideoSessionKHR(VkDevice _device,
                            VkVideoSessionKHR _session,
                            const VkAllocationCallbacks *pAllocator)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   RADV_FROM_HANDLE(radv_video_session, vid, _session);
   if (!_session)
      return;

   vk_object_base_finish(&vid->vk.base);
   vk_free2(&device->vk.alloc, pAllocator, vid);
}


VkResult
radv_CreateVideoSessionParametersKHR(VkDevice _device,
                                     const VkVideoSessionParametersCreateInfoKHR *pCreateInfo,
                                     const VkAllocationCallbacks *pAllocator,
                                     VkVideoSessionParametersKHR *pVideoSessionParameters)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   RADV_FROM_HANDLE(radv_video_session, vid, pCreateInfo->videoSession);
   RADV_FROM_HANDLE(radv_video_session_params, templ, pCreateInfo->videoSessionParametersTemplate);
   struct radv_video_session_params *params =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(*params), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!params)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = vk_video_session_parameters_init(&device->vk,
                                                      &params->vk,
                                                      &vid->vk,
                                                      templ ? &templ->vk : NULL,
                                                      pCreateInfo);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, pAllocator, params);
      return result;
   }

   *pVideoSessionParameters = radv_video_session_params_to_handle(params);
   return VK_SUCCESS;
}

void
radv_DestroyVideoSessionParametersKHR(VkDevice _device,
                                      VkVideoSessionParametersKHR _params,
                                      const VkAllocationCallbacks *pAllocator)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   RADV_FROM_HANDLE(radv_video_session_params, params, _params);

   vk_video_session_parameters_finish(&device->vk, &params->vk);
   vk_free2(&device->vk.alloc, pAllocator, params);
}

VkResult
radv_GetPhysicalDeviceVideoCapabilitiesKHR(VkPhysicalDevice physicalDevice,
                                           const VkVideoProfileInfoKHR *pVideoProfile,
                                           VkVideoCapabilitiesKHR *pCapabilities)
{
   RADV_FROM_HANDLE(radv_physical_device, pdevice, physicalDevice);
   struct video_codec_cap *cap = NULL;

   switch (pVideoProfile->videoCodecOperation) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
      cap = &pdevice->rad_info.dec_caps.codec_info[RADV_VIDEO_FORMAT_MPEG4_AVC];
      break;
   default:
      unreachable("unsupported operation");
   }

   if (cap && !cap->valid)
      cap = NULL;

   pCapabilities->flags = 0;
   pCapabilities->minBitstreamBufferOffsetAlignment = 128;
   pCapabilities->minBitstreamBufferSizeAlignment = 128;
   pCapabilities->pictureAccessGranularity.width = VL_MACROBLOCK_WIDTH;
   pCapabilities->pictureAccessGranularity.height = VL_MACROBLOCK_HEIGHT;
   pCapabilities->minCodedExtent.width = VL_MACROBLOCK_WIDTH;
   pCapabilities->minCodedExtent.height = VL_MACROBLOCK_HEIGHT;

   struct VkVideoDecodeCapabilitiesKHR *dec_caps = (struct VkVideoDecodeCapabilitiesKHR *)
      vk_find_struct(pCapabilities->pNext, VIDEO_DECODE_CAPABILITIES_KHR);
   if (dec_caps)
      dec_caps->flags = VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR;

   switch (pVideoProfile->videoCodecOperation) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR: {
      struct VkVideoDecodeH264CapabilitiesKHR *ext = (struct VkVideoDecodeH264CapabilitiesKHR *)
         vk_find_struct(pCapabilities->pNext, VIDEO_DECODE_H264_CAPABILITIES_KHR);
      pCapabilities->maxDpbSlots = NUM_H264_REFS;
      pCapabilities->maxActiveReferencePictures = NUM_H264_REFS;

      ext->fieldOffsetGranularity.x = 0;
      ext->fieldOffsetGranularity.y = 0;
      ext->maxLevelIdc = 51;
      strcpy(pCapabilities->stdHeaderVersion.extensionName, VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME);
      pCapabilities->stdHeaderVersion.specVersion = VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION;
      break;
   }
   default:
      break;
   }
   if (cap) {
      pCapabilities->maxCodedExtent.width = cap->max_width;
      pCapabilities->maxCodedExtent.height = cap->max_height;
   } else {
      switch (pVideoProfile->videoCodecOperation) {
      case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
         pCapabilities->maxCodedExtent.width = (pdevice->rad_info.family < CHIP_TONGA) ? 2048 : 4096;
         pCapabilities->maxCodedExtent.height = (pdevice->rad_info.family < CHIP_TONGA) ? 1152 : 4096;
         break;
      case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR:
         pCapabilities->maxCodedExtent.width = (pdevice->rad_info.family < CHIP_RENOIR) ?
            ((pdevice->rad_info.family < CHIP_TONGA) ? 2048 : 4096) : 8192;
         pCapabilities->maxCodedExtent.height = (pdevice->rad_info.family < CHIP_RENOIR) ?
            ((pdevice->rad_info.family < CHIP_TONGA) ? 1152 : 4096) : 4352;
         break;
      default:
         break;
      }
   }

   return VK_SUCCESS;
}

VkResult
radv_GetPhysicalDeviceVideoFormatPropertiesKHR(VkPhysicalDevice physicalDevice,
                                               const VkPhysicalDeviceVideoFormatInfoKHR *pVideoFormatInfo,
                                               uint32_t *pVideoFormatPropertyCount,
                                               VkVideoFormatPropertiesKHR *pVideoFormatProperties)
{
   /* radv requires separate allocates for DPB and decode video. */
   if ((pVideoFormatInfo->imageUsage & (VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
                                        VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR)) ==
       (VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   *pVideoFormatPropertyCount = 1;

   if (!pVideoFormatProperties)
      return VK_SUCCESS;

   pVideoFormatProperties[0].format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
   pVideoFormatProperties[0].imageType = VK_IMAGE_TYPE_2D;
   pVideoFormatProperties[0].imageTiling = VK_IMAGE_TILING_OPTIMAL;
   pVideoFormatProperties[0].imageUsageFlags = pVideoFormatInfo->imageUsage;
   return VK_SUCCESS;
}

#define RADV_BIND_SESSION_CTX 0
#define RADV_BIND_DECODER_CTX 1

VkResult
radv_GetVideoSessionMemoryRequirementsKHR(VkDevice _device,
                                          VkVideoSessionKHR videoSession,
                                          uint32_t *pMemoryRequirementsCount,
                                          VkVideoSessionMemoryRequirementsKHR *pMemoryRequirements)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   RADV_FROM_HANDLE(radv_video_session, vid, videoSession);
   uint32_t memory_type_bits = (1u << device->physical_device->memory_properties.memoryTypeCount) - 1;
   uint32_t num_memory_reqs = 1;
   int idx = 0;

   if (vid->stream_type == RDECODE_CODEC_H264_PERF)
      num_memory_reqs++;

   *pMemoryRequirementsCount = num_memory_reqs;

   if (!pMemoryRequirements)
      return VK_SUCCESS;

   /* 1 buffer for session context */
   pMemoryRequirements[idx].memoryBindIndex = RADV_BIND_SESSION_CTX;
   pMemoryRequirements[idx].memoryRequirements.size = RDECODE_SESSION_CONTEXT_SIZE;
   pMemoryRequirements[idx].memoryRequirements.alignment = 0;
   pMemoryRequirements[idx].memoryRequirements.memoryTypeBits = memory_type_bits;
   idx++;

   if (vid->stream_type == RDECODE_CODEC_H264_PERF) {
      pMemoryRequirements[idx].memoryBindIndex = RADV_BIND_DECODER_CTX;
      pMemoryRequirements[idx].memoryRequirements.size = calc_ctx_size_h264_perf(vid);
      pMemoryRequirements[idx].memoryRequirements.alignment = 0;
      pMemoryRequirements[idx].memoryRequirements.memoryTypeBits = memory_type_bits;
   }

   return VK_SUCCESS;
}

VkResult
radv_UpdateVideoSessionParametersKHR(VkDevice _device,
                                     VkVideoSessionParametersKHR videoSessionParameters,
                                     const VkVideoSessionParametersUpdateInfoKHR *pUpdateInfo)
{
   RADV_FROM_HANDLE(radv_video_session_params, params, videoSessionParameters);

   return vk_video_session_parameters_update(&params->vk, pUpdateInfo);
}

static void
copy_bind(struct radv_vid_mem *dst,
          const VkBindVideoSessionMemoryInfoKHR *src)
{
   dst->mem = radv_device_memory_from_handle(src->memory);
   dst->offset = src->memoryOffset;
   dst->size = src->memorySize;
}

VkResult
radv_BindVideoSessionMemoryKHR(VkDevice _device,
                               VkVideoSessionKHR videoSession,
                               uint32_t videoSessionBindMemoryCount,
                               const VkBindVideoSessionMemoryInfoKHR *pBindSessionMemoryInfos)
{
   RADV_FROM_HANDLE(radv_video_session, vid, videoSession);

   for (unsigned i = 0; i < videoSessionBindMemoryCount; i++) {
      switch (pBindSessionMemoryInfos[i].memoryBindIndex) {
      case RADV_BIND_SESSION_CTX:
         copy_bind(&vid->sessionctx, &pBindSessionMemoryInfos[i]);
         break;
      case RADV_BIND_DECODER_CTX:
         copy_bind(&vid->ctx, &pBindSessionMemoryInfos[i]);
         break;
      default:
         assert(0);
         break;
      }
   }
   return VK_SUCCESS;
}

/* add a new set register command to the IB */
static void set_reg(struct radv_cmd_buffer *cmd_buffer, unsigned reg, uint32_t val)
{
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   radeon_emit(cs, RDECODE_PKT0(reg >> 2, 0));
   radeon_emit(cs, val);
}

static void send_cmd(struct radv_cmd_buffer *cmd_buffer, unsigned cmd,
                     struct radeon_winsys_bo *bo, uint32_t offset)
{
   struct radv_physical_device *pdev = cmd_buffer->device->physical_device;
   uint64_t addr;

   radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs, bo);
   addr = radv_buffer_get_va(bo);
   addr += offset;
   set_reg(cmd_buffer, pdev->vid_dec_reg.data0, addr);
   set_reg(cmd_buffer, pdev->vid_dec_reg.data1, addr >> 32);
   set_reg(cmd_buffer, pdev->vid_dec_reg.cmd, cmd << 1);
}

static void rvcn_dec_message_create(struct radv_video_session *vid,
                                    void *ptr, uint32_t size)
{
   rvcn_dec_message_header_t *header = ptr;
   rvcn_dec_message_create_t *create = (void *)((char *)ptr + sizeof(rvcn_dec_message_header_t));

   memset(ptr, 0, size);
   header->header_size = sizeof(rvcn_dec_message_header_t);
   header->total_size = size;
   header->num_buffers = 1;
   header->msg_type = RDECODE_MSG_CREATE;
   header->stream_handle = vid->stream_handle;
   header->status_report_feedback_number = 0;

   header->index[0].message_id = RDECODE_MESSAGE_CREATE;
   header->index[0].offset = sizeof(rvcn_dec_message_header_t);
   header->index[0].size = sizeof(rvcn_dec_message_create_t);
   header->index[0].filled = 0;

   create->stream_type = vid->stream_type;
   create->session_flags = 0;
   create->width_in_samples = vid->vk.max_coded.width;
   create->height_in_samples = vid->vk.max_coded.height;
}

static void rvcn_dec_message_feedback(void *ptr)
{
   rvcn_dec_feedback_header_t *header = (void *)ptr;

   header->header_size = sizeof(rvcn_dec_feedback_header_t);
   header->total_size = sizeof(rvcn_dec_feedback_header_t);
   header->num_buffers = 0;
}

static rvcn_dec_message_avc_t get_h264_msg(struct radv_video_session *vid,
                                           struct radv_video_session_params *params,
                                           const struct VkVideoDecodeInfoKHR *frame_info,
                                           uint32_t *slice_offset,
                                           uint32_t *width_in_samples,
                                           uint32_t *height_in_samples,
                                           void *it_ptr)
{
   rvcn_dec_message_avc_t result;
   const struct VkVideoDecodeH264PictureInfoKHR *h264_pic_info =
      vk_find_struct_const(frame_info->pNext, VIDEO_DECODE_H264_PICTURE_INFO_KHR);

   *slice_offset = h264_pic_info->pSliceOffsets[0];

   memset(&result, 0, sizeof(result));

   assert(params->vk.h264_dec.std_sps_count > 0);
   const StdVideoH264SequenceParameterSet *sps = vk_video_find_h264_dec_std_sps(&params->vk, h264_pic_info->pStdPictureInfo->seq_parameter_set_id);
   switch (sps->profile_idc) {
   case STD_VIDEO_H264_PROFILE_IDC_BASELINE:
      result.profile = RDECODE_H264_PROFILE_BASELINE;
      break;
   case STD_VIDEO_H264_PROFILE_IDC_MAIN:
      result.profile = RDECODE_H264_PROFILE_MAIN;
      break;
   case STD_VIDEO_H264_PROFILE_IDC_HIGH:
      result.profile = RDECODE_H264_PROFILE_HIGH;
      break;
   default:
      fprintf(stderr, "UNSUPPORTED CODEC %d\n", sps->profile_idc);
      result.profile= RDECODE_H264_PROFILE_MAIN;
      break;
   }

   *width_in_samples = (sps->pic_width_in_mbs_minus1 + 1) * 16;
   *height_in_samples = (sps->pic_height_in_map_units_minus1 + 1) * 16;
   result.level = sps->level_idc;

   result.sps_info_flags = 0;

   result.sps_info_flags |= sps->flags.direct_8x8_inference_flag << 0;
   result.sps_info_flags |= sps->flags.mb_adaptive_frame_field_flag << 1;
   result.sps_info_flags |= sps->flags.frame_mbs_only_flag << 2;
   result.sps_info_flags |= sps->flags.delta_pic_order_always_zero_flag << 3;
   result.sps_info_flags |= 1 << RDECODE_SPS_INFO_H264_EXTENSION_SUPPORT_FLAG_SHIFT;

   result.bit_depth_luma_minus8 = sps->bit_depth_luma_minus8;
   result.bit_depth_chroma_minus8 = sps->bit_depth_chroma_minus8;
   result.log2_max_frame_num_minus4 = sps->log2_max_frame_num_minus4;
   result.pic_order_cnt_type = sps->pic_order_cnt_type;
   result.log2_max_pic_order_cnt_lsb_minus4 = sps->log2_max_pic_order_cnt_lsb_minus4;

   result.chroma_format = sps->chroma_format_idc;

   const StdVideoH264PictureParameterSet *pps = vk_video_find_h264_dec_std_pps(&params->vk, h264_pic_info->pStdPictureInfo->pic_parameter_set_id);
   result.pps_info_flags = 0;
   result.pps_info_flags |= pps->flags.transform_8x8_mode_flag << 0;
   result.pps_info_flags |= pps->flags.redundant_pic_cnt_present_flag << 1;
   result.pps_info_flags |= pps->flags.constrained_intra_pred_flag << 2;
   result.pps_info_flags |= pps->flags.deblocking_filter_control_present_flag << 3;
   result.pps_info_flags |= pps->weighted_bipred_idc << 4;
   result.pps_info_flags |= pps->flags.weighted_pred_flag << 6;
   result.pps_info_flags |= pps->flags.bottom_field_pic_order_in_frame_present_flag << 7;
   result.pps_info_flags |= pps->flags.entropy_coding_mode_flag << 8;

   result.pic_init_qp_minus26 = pps->pic_init_qp_minus26;
   result.chroma_qp_index_offset = pps->chroma_qp_index_offset;
   result.second_chroma_qp_index_offset = pps->second_chroma_qp_index_offset;

   if (pps->flags.pic_scaling_matrix_present_flag) {
      memcpy(result.scaling_list_4x4, pps->pScalingLists->ScalingList4x4, 6 * 16);
      memcpy(result.scaling_list_8x8[0], pps->pScalingLists->ScalingList8x8[0], 64);
      memcpy(result.scaling_list_8x8[1], pps->pScalingLists->ScalingList8x8[3], 64);
   } else if (sps->flags.seq_scaling_matrix_present_flag) {
      memcpy(result.scaling_list_4x4, sps->pScalingLists->ScalingList4x4, 6 * 16);
      memcpy(result.scaling_list_8x8[0], sps->pScalingLists->ScalingList8x8[0], 64);
      memcpy(result.scaling_list_8x8[1], sps->pScalingLists->ScalingList8x8[3], 64);
   } else {
      memset(result.scaling_list_4x4, 0x10, 6*16);
      memset(result.scaling_list_8x8, 0x10, 2*64);
   }

   memset(it_ptr, 0, IT_SCALING_TABLE_SIZE);
   memcpy(it_ptr, result.scaling_list_4x4, 6 * 16);
   memcpy((char *)it_ptr + 96, result.scaling_list_8x8, 2 * 64);

   result.num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_default_active_minus1;
   result.num_ref_idx_l1_active_minus1 = pps->num_ref_idx_l1_default_active_minus1;

   result.curr_field_order_cnt_list[0] = h264_pic_info->pStdPictureInfo->PicOrderCnt[0];
   result.curr_field_order_cnt_list[1] = h264_pic_info->pStdPictureInfo->PicOrderCnt[1];

   result.frame_num = h264_pic_info->pStdPictureInfo->frame_num;

   result.num_ref_frames = sps->max_num_ref_frames;
   for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
      int idx = frame_info->pReferenceSlots[i].slotIndex;
      const struct VkVideoDecodeH264DpbSlotInfoKHR *dpb_slot =
         vk_find_struct_const(frame_info->pReferenceSlots[i].pNext, VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR);

      result.frame_num_list[idx] = idx;
      result.field_order_cnt_list[idx][0] = dpb_slot->pStdReferenceInfo->PicOrderCnt[0];
      result.field_order_cnt_list[idx][1] = dpb_slot->pStdReferenceInfo->PicOrderCnt[1];
   }
   result.decoded_pic_idx = frame_info->pSetupReferenceSlot->slotIndex;

   return result;
}

static bool rvcn_dec_message_decode(struct radv_video_session *vid,
                                    struct radv_video_session_params *params,
                                    void *ptr,
                                    void *it_ptr,
                                    uint32_t *slice_offset,
                                    const struct VkVideoDecodeInfoKHR *frame_info)
{
   rvcn_dec_message_header_t *header;
   rvcn_dec_message_index_t *index_codec;
   rvcn_dec_message_decode_t *decode;
   void *codec;
   unsigned sizes = 0, offset_decode, offset_codec;
   struct radv_image_view *dst_iv = radv_image_view_from_handle(frame_info->dstPictureResource.imageViewBinding);
   struct radv_image *img = dst_iv->image;
   struct radv_image_plane *luma = &img->planes[0];
   struct radv_image_plane *chroma = &img->planes[1];
   struct radv_image_view *dpb_iv = radv_image_view_from_handle(frame_info->pSetupReferenceSlot->pPictureResource->imageViewBinding);
   struct radv_image *dpb = dpb_iv->image;

   header = ptr;
   sizes += sizeof(rvcn_dec_message_header_t);

   index_codec = (void *)((char *)header + sizes);
   sizes += sizeof(rvcn_dec_message_index_t);

   offset_decode = sizes;
   decode = (void *)((char*)header + sizes);
   sizes += sizeof(rvcn_dec_message_decode_t);

   offset_codec = sizes;
   codec = (void *)((char *)header + sizes);

   memset(ptr, 0, sizes);

   header->header_size = sizeof(rvcn_dec_message_header_t);
   header->total_size = sizes;
   header->msg_type = RDECODE_MSG_DECODE;
   header->stream_handle = vid->stream_handle;
   header->status_report_feedback_number = vid->dbg_frame_cnt++;

   header->index[0].message_id = RDECODE_MESSAGE_DECODE;
   header->index[0].offset = offset_decode;
   header->index[0].size = sizeof(rvcn_dec_message_decode_t);
   header->index[0].filled = 0;
   header->num_buffers = 1;

   index_codec->offset = offset_codec;
   index_codec->size = sizeof(rvcn_dec_message_avc_t);
   index_codec->filled = 0;
   ++header->num_buffers;

   decode->stream_type = vid->stream_type;
   decode->decode_flags = 0;
   decode->width_in_samples = dst_iv->image->vk.extent.width;
   decode->height_in_samples = dst_iv->image->vk.extent.height;

   decode->bsd_size = frame_info->srcBufferRange;

   decode->dpb_size = (vid->dpb_type != DPB_DYNAMIC_TIER_2) ? dpb->size : 0;

   decode->dt_size = dst_iv->image->planes[0].surface.total_size +
      dst_iv->image->planes[1].surface.total_size;
   decode->sct_size = 0;
   decode->sc_coeff_size = 0;

   decode->sw_ctxt_size = RDECODE_SESSION_CONTEXT_SIZE;
   decode->db_pitch = align(frame_info->dstPictureResource.codedExtent.width, vid->db_alignment);

   decode->db_surf_tile_config = 0;

   decode->dt_pitch = luma->surface.u.gfx9.surf_pitch * luma->surface.blk_w;
   decode->dt_uv_pitch = chroma->surface.u.gfx9.surf_pitch * chroma->surface.blk_w;

   if (luma->surface.meta_offset) {
      fprintf(stderr, "DCC SURFACES NOT SUPPORTED.\n");
      return false;
   }

   decode->dt_tiling_mode = 0;
   decode->dt_swizzle_mode = luma->surface.u.gfx9.swizzle_mode;
   decode->dt_array_mode = RDECODE_ARRAY_MODE_LINEAR;
   decode->dt_field_mode = vid->interlaced ? 1 : 0;
   decode->dt_surf_tile_config = 0;
   decode->dt_uv_surf_tile_config = 0;

   decode->dt_luma_top_offset = luma->surface.u.gfx9.surf_offset;
   decode->dt_chroma_top_offset = chroma->surface.u.gfx9.surf_offset;

   if (decode->dt_field_mode) {
      decode->dt_luma_bottom_offset =
         luma->surface.u.gfx9.surf_offset + luma->surface.u.gfx9.surf_slice_size;
      decode->dt_chroma_bottom_offset =
         chroma->surface.u.gfx9.surf_offset + chroma->surface.u.gfx9.surf_slice_size;
   } else {
      decode->dt_luma_bottom_offset = decode->dt_luma_top_offset;
      decode->dt_chroma_bottom_offset = decode->dt_chroma_top_offset;
   }

   *slice_offset = 0;
   switch (vid->vk.op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR: {
      rvcn_dec_message_avc_t avc = get_h264_msg(vid, params, frame_info, slice_offset, &decode->width_in_samples, &decode->height_in_samples, it_ptr);
      memcpy(codec, (void *)&avc, sizeof(rvcn_dec_message_avc_t));
      index_codec->message_id = RDECODE_MESSAGE_AVC;
      break;
   }
   default:
      unreachable("unknown operation");
   }

   decode->hw_ctxt_size = vid->ctx.size;

   return true;
}

void
radv_CmdBeginVideoCodingKHR(VkCommandBuffer commandBuffer,
                            const VkVideoBeginCodingInfoKHR *pBeginInfo)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_video_session, vid, pBeginInfo->videoSession);
   RADV_FROM_HANDLE(radv_video_session_params, params, pBeginInfo->videoSessionParameters);

   cmd_buffer->video.vid = vid;
   cmd_buffer->video.params = params;
}

static void
radv_vcn_cmd_reset(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_video_session *vid = cmd_buffer->video.vid;
   uint32_t size = sizeof(rvcn_dec_message_header_t) + sizeof(rvcn_dec_message_create_t);

   void *ptr;
   uint32_t out_offset;
   radv_vid_buffer_upload_alloc(cmd_buffer, size, &out_offset,
                                &ptr);

   rvcn_dec_message_create(vid, ptr, size);
   send_cmd(cmd_buffer, RDECODE_CMD_SESSION_CONTEXT_BUFFER, vid->sessionctx.mem->bo, vid->sessionctx.offset);
   send_cmd(cmd_buffer, RDECODE_CMD_MSG_BUFFER, cmd_buffer->upload.upload_bo, out_offset);
   /* pad out the IB to the 16 dword boundary - otherwise the fw seems to be unhappy */
   for (unsigned i = 0; i < 8; i++)
      radeon_emit(cmd_buffer->cs, 0x81ff);
}

void
radv_CmdControlVideoCodingKHR(VkCommandBuffer commandBuffer,
                              const VkVideoCodingControlInfoKHR *pCodingControlInfo)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   if (pCodingControlInfo->flags & VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR) {
      radv_vcn_cmd_reset(cmd_buffer);
   }
}

void
radv_CmdEndVideoCodingKHR(VkCommandBuffer commandBuffer,
                          const VkVideoEndCodingInfoKHR *pEndCodingInfo)
{
}

static void
radv_vcn_decode_video(struct radv_cmd_buffer *cmd_buffer,
                      const VkVideoDecodeInfoKHR *frame_info)
{
   RADV_FROM_HANDLE(radv_buffer, src_buffer, frame_info->srcBuffer);
   struct radv_video_session *vid = cmd_buffer->video.vid;
   struct radv_video_session_params *params = cmd_buffer->video.params;
   unsigned size = 0;
   void *ptr, *fb_ptr, *it_ptr = NULL;
   uint32_t out_offset, fb_offset, it_offset = 0;
   struct radeon_winsys_bo *msg_bo, *fb_bo, *it_bo = NULL;

   size += sizeof(rvcn_dec_message_header_t);
   size += sizeof(rvcn_dec_message_index_t);
   size += sizeof(rvcn_dec_message_decode_t);
   switch (vid->vk.op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
      size += sizeof(rvcn_dec_message_avc_t);
      break;
   default:
      unreachable("unsupported codec.");
   }

   radv_vid_buffer_upload_alloc(cmd_buffer, FB_BUFFER_SIZE, &fb_offset,
                                &fb_ptr);
   fb_bo = cmd_buffer->upload.upload_bo;
   if (have_it(vid)) {
      radv_vid_buffer_upload_alloc(cmd_buffer, IT_SCALING_TABLE_SIZE, &it_offset,
                                   &it_ptr);
      it_bo = cmd_buffer->upload.upload_bo;
   }

   radv_vid_buffer_upload_alloc(cmd_buffer, size, &out_offset,
                                &ptr);
   msg_bo = cmd_buffer->upload.upload_bo;

   uint32_t slice_offset;	  
   rvcn_dec_message_decode(vid, params, ptr, it_ptr, &slice_offset, frame_info);
   rvcn_dec_message_feedback(fb_ptr);
   send_cmd(cmd_buffer, RDECODE_CMD_SESSION_CONTEXT_BUFFER, vid->sessionctx.mem->bo, vid->sessionctx.offset);
   send_cmd(cmd_buffer, RDECODE_CMD_MSG_BUFFER, msg_bo, out_offset);

   if (vid->dpb_type != DPB_DYNAMIC_TIER_2) {
      struct radv_image_view *dpb_iv = radv_image_view_from_handle(frame_info->pSetupReferenceSlot->pPictureResource->imageViewBinding);
      struct radv_image *dpb = dpb_iv->image;
      send_cmd(cmd_buffer, RDECODE_CMD_DPB_BUFFER, dpb->bindings[0].bo, dpb->bindings[0].offset);
   }

   if (vid->ctx.mem)
      send_cmd(cmd_buffer, RDECODE_CMD_CONTEXT_BUFFER, vid->ctx.mem->bo, vid->ctx.offset);

   send_cmd(cmd_buffer, RDECODE_CMD_BITSTREAM_BUFFER, src_buffer->bo, src_buffer->offset + frame_info->srcBufferOffset + slice_offset);

   struct radv_image_view *dst_iv = radv_image_view_from_handle(frame_info->dstPictureResource.imageViewBinding);
   struct radv_image *img = dst_iv->image;
   send_cmd(cmd_buffer, RDECODE_CMD_DECODING_TARGET_BUFFER, img->bindings[0].bo, img->bindings[0].offset);
   send_cmd(cmd_buffer, RDECODE_CMD_FEEDBACK_BUFFER, fb_bo, fb_offset);
   if (have_it(vid))
      send_cmd(cmd_buffer, RDECODE_CMD_IT_SCALING_TABLE_BUFFER, it_bo, it_offset);

   set_reg(cmd_buffer, cmd_buffer->device->physical_device->vid_dec_reg.cntl, 1);
}

void
radv_CmdDecodeVideoKHR(VkCommandBuffer commandBuffer,
                       const VkVideoDecodeInfoKHR *frame_info)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   assert (cmd_buffer->device->physical_device->rad_info.ip[AMD_IP_VCN_DEC].num_queues > 0);

   radv_vcn_decode_video(cmd_buffer, frame_info);
}