/*
 * Copyright 2009, VMware, Inc.
 * Copyright (C) 2010 LunarG Inc.
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "st_interop.h"
#include "st_cb_texture.h"
#include "st_texture.h"

#include "bufferobj.h"
#include "texobj.h"

int
st_interop_query_device_info(struct st_context_iface *st,
                             struct mesa_glinterop_device_info *out)
{
   struct pipe_screen *screen = st->pipe->screen;

   /* There is no version 0, thus we do not support it */
   if (out->version == 0)
      return MESA_GLINTEROP_INVALID_VERSION;

   out->pci_segment_group = screen->get_param(screen, PIPE_CAP_PCI_GROUP);
   out->pci_bus = screen->get_param(screen, PIPE_CAP_PCI_BUS);
   out->pci_device = screen->get_param(screen, PIPE_CAP_PCI_DEVICE);
   out->pci_function = screen->get_param(screen, PIPE_CAP_PCI_FUNCTION);

   out->vendor_id = screen->get_param(screen, PIPE_CAP_VENDOR_ID);
   out->device_id = screen->get_param(screen, PIPE_CAP_DEVICE_ID);

   /* Instruct the caller that we support up-to version one of the interface */
   out->version = 1;

   return MESA_GLINTEROP_SUCCESS;
}

int
st_interop_export_object(struct st_context_iface *st,
                         struct mesa_glinterop_export_in *in,
                         struct mesa_glinterop_export_out *out)
{
   struct pipe_screen *screen = st->pipe->screen;
   struct gl_context *ctx = ((struct st_context *)st)->ctx;
   struct pipe_resource *res = NULL;
   struct winsys_handle whandle;
   unsigned target, usage;
   boolean success;

   /* There is no version 0, thus we do not support it */
   if (in->version == 0 || out->version == 0)
      return MESA_GLINTEROP_INVALID_VERSION;

   /* Validate the target. */
   switch (in->target) {
   case GL_TEXTURE_BUFFER:
   case GL_TEXTURE_1D:
   case GL_TEXTURE_2D:
   case GL_TEXTURE_3D:
   case GL_TEXTURE_RECTANGLE:
   case GL_TEXTURE_1D_ARRAY:
   case GL_TEXTURE_2D_ARRAY:
   case GL_TEXTURE_CUBE_MAP_ARRAY:
   case GL_TEXTURE_CUBE_MAP:
   case GL_TEXTURE_2D_MULTISAMPLE:
   case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
   case GL_TEXTURE_EXTERNAL_OES:
   case GL_RENDERBUFFER:
   case GL_ARRAY_BUFFER:
      target = in->target;
      break;
   case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
   case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
   case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
   case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
   case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
   case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
      target = GL_TEXTURE_CUBE_MAP;
      break;
   default:
      return MESA_GLINTEROP_INVALID_TARGET;
   }

   /* Validate the simple case of miplevel. */
   if ((target == GL_RENDERBUFFER || target == GL_ARRAY_BUFFER) &&
       in->miplevel != 0)
      return MESA_GLINTEROP_INVALID_MIP_LEVEL;

   /* Wait for glthread to finish to get up-to-date GL object lookups. */
   if (st->thread_finish)
      st->thread_finish(st);

   /* Validate the OpenGL object and get pipe_resource. */
   simple_mtx_lock(&ctx->Shared->Mutex);

   if (target == GL_ARRAY_BUFFER) {
      /* Buffer objects.
      *
      * The error checking is based on the documentation of
      * clCreateFromGLBuffer from OpenCL 2.0 SDK.
      */
      struct gl_buffer_object *buf = _mesa_lookup_bufferobj(ctx, in->obj);

      /* From OpenCL 2.0 SDK, clCreateFromGLBuffer:
      *  "CL_INVALID_GL_OBJECT if bufobj is not a GL buffer object or is
      *   a GL buffer object but does not have an existing data store or
      *   the size of the buffer is 0."
      */
      if (!buf || buf->Size == 0) {
         simple_mtx_unlock(&ctx->Shared->Mutex);
         return MESA_GLINTEROP_INVALID_OBJECT;
      }

      res = buf->buffer;
      if (!res) {
         /* this shouldn't happen */
         simple_mtx_unlock(&ctx->Shared->Mutex);
         return MESA_GLINTEROP_INVALID_OBJECT;
      }

      out->buf_offset = 0;
      out->buf_size = buf->Size;

      buf->UsageHistory |= USAGE_DISABLE_MINMAX_CACHE;
   } else if (target == GL_RENDERBUFFER) {
      /* Renderbuffers.
      *
      * The error checking is based on the documentation of
      * clCreateFromGLRenderbuffer from OpenCL 2.0 SDK.
      */
      struct gl_renderbuffer *rb = _mesa_lookup_renderbuffer(ctx, in->obj);

      /* From OpenCL 2.0 SDK, clCreateFromGLRenderbuffer:
      *   "CL_INVALID_GL_OBJECT if renderbuffer is not a GL renderbuffer
      *    object or if the width or height of renderbuffer is zero."
      */
      if (!rb || rb->Width == 0 || rb->Height == 0) {
         simple_mtx_unlock(&ctx->Shared->Mutex);
         return MESA_GLINTEROP_INVALID_OBJECT;
      }

      /* From OpenCL 2.0 SDK, clCreateFromGLRenderbuffer:
      *   "CL_INVALID_OPERATION if renderbuffer is a multi-sample GL
      *    renderbuffer object."
      */
      if (rb->NumSamples > 1) {
         simple_mtx_unlock(&ctx->Shared->Mutex);
         return MESA_GLINTEROP_INVALID_OPERATION;
      }

      /* From OpenCL 2.0 SDK, clCreateFromGLRenderbuffer:
      *   "CL_OUT_OF_RESOURCES if there is a failure to allocate resources
      *    required by the OpenCL implementation on the device."
      */
      res = rb->texture;
      if (!res) {
         simple_mtx_unlock(&ctx->Shared->Mutex);
         return MESA_GLINTEROP_OUT_OF_RESOURCES;
      }

      out->internal_format = rb->InternalFormat;
      out->view_minlevel = 0;
      out->view_numlevels = 1;
      out->view_minlayer = 0;
      out->view_numlayers = 1;
   } else {
      /* Texture objects.
      *
      * The error checking is based on the documentation of
      * clCreateFromGLTexture from OpenCL 2.0 SDK.
      */
      struct gl_texture_object *obj = _mesa_lookup_texture(ctx, in->obj);

      if (obj)
         _mesa_test_texobj_completeness(ctx, obj);

      /* From OpenCL 2.0 SDK, clCreateFromGLTexture:
      *   "CL_INVALID_GL_OBJECT if texture is not a GL texture object whose
      *    type matches texture_target, if the specified miplevel of texture
      *    is not defined, or if the width or height of the specified
      *    miplevel is zero or if the GL texture object is incomplete."
      */
      if (!obj ||
          obj->Target != target ||
          !obj->_BaseComplete ||
          (in->miplevel > 0 && !obj->_MipmapComplete)) {
         simple_mtx_unlock(&ctx->Shared->Mutex);
         return MESA_GLINTEROP_INVALID_OBJECT;
      }

      if (target == GL_TEXTURE_BUFFER) {
         struct gl_buffer_object *stBuf =
            obj->BufferObject;

         if (!stBuf || !stBuf->buffer) {
            /* this shouldn't happen */
            simple_mtx_unlock(&ctx->Shared->Mutex);
            return MESA_GLINTEROP_INVALID_OBJECT;
         }
         res = stBuf->buffer;

         out->internal_format = obj->BufferObjectFormat;
         out->buf_offset = obj->BufferOffset;
         out->buf_size = obj->BufferSize == -1 ? obj->BufferObject->Size :
            obj->BufferSize;

         obj->BufferObject->UsageHistory |= USAGE_DISABLE_MINMAX_CACHE;
      } else {
         /* From OpenCL 2.0 SDK, clCreateFromGLTexture:
         *   "CL_INVALID_MIP_LEVEL if miplevel is less than the value of
         *    levelbase (for OpenGL implementations) or zero (for OpenGL ES
         *    implementations); or greater than the value of q (for both OpenGL
         *    and OpenGL ES). levelbase and q are defined for the texture in
         *    section 3.8.10 (Texture Completeness) of the OpenGL 2.1
         *    specification and section 3.7.10 of the OpenGL ES 2.0."
         */
         if (in->miplevel < obj->Attrib.BaseLevel || in->miplevel > obj->_MaxLevel) {
            simple_mtx_unlock(&ctx->Shared->Mutex);
            return MESA_GLINTEROP_INVALID_MIP_LEVEL;
         }

         if (!st_finalize_texture(ctx, st->pipe, obj, 0)) {
            simple_mtx_unlock(&ctx->Shared->Mutex);
            return MESA_GLINTEROP_OUT_OF_RESOURCES;
         }

         res = st_get_texobj_resource(obj);
         if (!res) {
            /* Incomplete texture buffer object? This shouldn't really occur. */
            simple_mtx_unlock(&ctx->Shared->Mutex);
            return MESA_GLINTEROP_INVALID_OBJECT;
         }

         out->internal_format = obj->Image[0][0]->InternalFormat;
         out->view_minlevel = obj->Attrib.MinLevel;
         out->view_numlevels = obj->Attrib.NumLevels;
         out->view_minlayer = obj->Attrib.MinLayer;
         out->view_numlayers = obj->Attrib.NumLayers;
      }
   }

   /* Get the handle. */
   switch (in->access) {
   case MESA_GLINTEROP_ACCESS_READ_ONLY:
      usage = 0;
      break;
   case MESA_GLINTEROP_ACCESS_READ_WRITE:
   case MESA_GLINTEROP_ACCESS_WRITE_ONLY:
      usage = PIPE_HANDLE_USAGE_SHADER_WRITE;
      break;
   default:
      usage = 0;
   }

   memset(&whandle, 0, sizeof(whandle));
   whandle.type = WINSYS_HANDLE_TYPE_FD;

   success = screen->resource_get_handle(screen, st->pipe, res, &whandle,
                                         usage);
   simple_mtx_unlock(&ctx->Shared->Mutex);

   if (!success)
      return MESA_GLINTEROP_OUT_OF_HOST_MEMORY;

#ifndef _WIN32
   out->dmabuf_fd = whandle.handle;
#else
   out->win32_handle = whandle.handle;
#endif
   out->out_driver_data_written = 0;

   if (res->target == PIPE_BUFFER)
      out->buf_offset += whandle.offset;

   /* Instruct the caller that we support up-to version one of the interface */
   in->version = 1;
   out->version = 1;

   return MESA_GLINTEROP_SUCCESS;
}