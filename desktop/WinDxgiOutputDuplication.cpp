// Copyright (C) 2012 GlavSoft LLC.
// All rights reserved.
//
//-------------------------------------------------------------------------
// This file is part of the TightVNC software.  Please visit our Web site:
//
//                       http://www.tightvnc.com/
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//-------------------------------------------------------------------------
//

#include "WinDxCriticalException.h"
#include "WinDxRecoverableException.h"
#include "rfb/StandardPixelFormatFactory.h"
#include "WinCursorShapeUtils.h"

// The header including of this cpp file must be at last place to avoid build conflicts.
#include "WinDxgiOutputDuplication.h"

WinDxgiOutputDuplication::WinDxgiOutputDuplication(WinDxgiOutput1 *dxgiOutput, WinD3D11Device *d3D11Device)
: m_outDupl(0)
{
  HRESULT hr = dxgiOutput->getDxgiOutput1()->DuplicateOutput(d3D11Device->getDevice(), &m_outDupl);
  if (FAILED(hr)) {
    if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
      throw WinDxRecoverableException(
        _T("Can't DuplicateOutput() because resource doesn't available at the time"), hr);
    } else if (hr == E_ACCESSDENIED) {
        throw WinDxRecoverableException(
          _T("Can't DuplicateOutput() because of access denied error"), hr);
    } else {
      throw WinDxCriticalException(_T("Can't DuplicateOutput()"), hr);
    }
  }
}

WinDxgiOutputDuplication::WinDxgiOutputDuplication(const WinDxgiOutputDuplication &src)
{
  copy(src);
}

WinDxgiOutputDuplication::~WinDxgiOutputDuplication()
{
  if (m_outDupl != 0) {
    m_outDupl->Release();
    m_outDupl = 0;
  }
}


WinDxgiOutputDuplication &WinDxgiOutputDuplication::operator = (WinDxgiOutputDuplication const &src)
{
  copy(src);
  return *this;
}

void WinDxgiOutputDuplication::copy(const WinDxgiOutputDuplication &src)
{
  if (this != &src) {
    m_outDupl = src.m_outDupl;
    m_outDupl->AddRef();
  }
}

IDXGIOutputDuplication *WinDxgiOutputDuplication::getDxgiOutputDuplication()
{
  return m_outDupl;
}

size_t WinDxgiOutputDuplication::getFrameMoveRects(std::vector<DXGI_OUTDUPL_MOVE_RECT> *moveRects)
{
  // Get move rect buffer size.
  char stub;
  UINT reqBufSize = 0;
  HRESULT hr;
  hr = m_outDupl->GetFrameMoveRects(reqBufSize, reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT *>(&stub), &reqBufSize);
  if (!FAILED(hr)) {
    return 0;
  } else if (hr != DXGI_ERROR_MORE_DATA) {
    throw WinDxException(_T("Can't get buffer size to get move rects"), hr);
  }

  size_t elementSize = sizeof((*moveRects)[0]);
  UINT bufSize = (UINT)(moveRects->size() * elementSize);
  if (reqBufSize > bufSize) {
    moveRects->resize(reqBufSize / elementSize);
    bufSize = (UINT)(moveRects->size() * elementSize);
  }

  // Get move rectangles.
  hr = m_outDupl->GetFrameMoveRects(bufSize, &moveRects->front(), &bufSize);
  if (FAILED(hr)) {
    throw WinDxException(_T("Can't get move rects"), hr);
  }
  return bufSize / elementSize;
}

size_t WinDxgiOutputDuplication::getFrameDirtyRects(std::vector<RECT> *dirtyRects)
{
  // Get dirty rect buffer size.
  char stub;
  UINT reqBufSize = 0;
  HRESULT hr;
  hr = m_outDupl->GetFrameDirtyRects(reqBufSize, reinterpret_cast<RECT *>(&stub), &reqBufSize);
  if (!FAILED(hr)) {
    return 0;
  } else if (hr != DXGI_ERROR_MORE_DATA) {
    throw WinDxException(_T("Can't get buffer size to get dirty rects"), hr);
  }

  size_t elementSize = sizeof((*dirtyRects)[0]);
  UINT bufSize = (UINT)(dirtyRects->size() * elementSize);
  if (reqBufSize > bufSize) {
    dirtyRects->resize(reqBufSize / elementSize);
    bufSize = (UINT)(dirtyRects->size() * elementSize);
  }

  // Get dirty rectangles.
  hr = m_outDupl->GetFrameDirtyRects(bufSize, &dirtyRects->front(), &bufSize);
  if (FAILED(hr)) {
    throw WinDxException(_T("Can't get dirty rects"), hr);
  }
  return bufSize / elementSize;
}

void WinDxgiOutputDuplication::getFrameCursorShape(CursorShape *cursorShape, UINT pointerShapeBufferSize, LogWriter *log)
{
  //log->debug(_T("%d"), pointerShapeBufferSize);
  // This function can calculate required buffer size by self but the size is already known.
  if (pointerShapeBufferSize == 0) {
	  cursorShape->resetToEmpty();
	  return;
  }
  HRESULT hr;
  UINT reqSize = 0;
  std::vector<char> buffer(pointerShapeBufferSize);
  DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo;
  hr = m_outDupl->GetFramePointerShape((UINT)buffer.size(), &buffer.front(), &reqSize, &shapeInfo);
  log->debug(_T("CursorShapeInfo: pounter info buffer size: %d, required: %d"), pointerShapeBufferSize, reqSize);
  if (FAILED(hr)) {
    throw WinDxException(_T("Can't get frame cursor shape with GetFramePointerShape() calling"), hr);
  }

  log->debug(_T("CursorShapeInfo: Type: %d"), shapeInfo.Type);
  log->debug(_T("CursorShapeInfo: Width: %d, Height: %d"), shapeInfo.Width, shapeInfo.Height);
  log->debug(_T("CursorShapeInfo: shapeInfo.HotSpot.x: %d, , shapeInfo.HotSpot.y: %d"), shapeInfo.HotSpot.x, shapeInfo.HotSpot.y);
  log->debug(_T("CursorShapeInfo: Pitch: %d"), shapeInfo.Pitch);

  buffer.resize(reqSize);

  CursorShape newCursorShape;
  PixelFormat pf = StandardPixelFormatFactory::create32bppPixelFormat();
  newCursorShape.setHotSpot(shapeInfo.HotSpot.x, shapeInfo.HotSpot.y);

  UINT pitch;
  Dimension dim;

  if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
    pitch = ((shapeInfo.Width + 15) / 16) * 2;
  }
  else {
    pitch = shapeInfo.Width * 4;
  }

  if (shapeInfo.Pitch > pitch) {
    WinCursorShapeUtils::trimBuffer(&buffer, &shapeInfo);
    pitch = shapeInfo.Pitch;
    log->debug(_T("Trimmed CursorShapeInfo: Width: %d, Height: %d"), shapeInfo.Width, shapeInfo.Height);
    log->debug(_T("Trimmed CursorShapeInfo: Pitch: %d"), shapeInfo.Pitch);
  } 

  if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
    dim.setDim(shapeInfo.Width, shapeInfo.Height / 2);
  }
  else {
    dim.setDim(shapeInfo.Width, shapeInfo.Height);
  }
  newCursorShape.setProperties(&dim, &pf);
      
  // monochrome cursor
  if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
    WinCursorShapeUtils::winMonoShapeToRfb(newCursorShape.getPixels(),
        &buffer.front(), &buffer[pitch * dim.height], pitch);
    newCursorShape.assignMaskFromWindows(&buffer.front());
	  cursorShape->clone(&newCursorShape);
	  return;
  } 

  // 32bpp cursor
  // Check buffer size.
  size_t shapeSize = newCursorShape.getPixelsSize();
  if (shapeSize > buffer.size()) {
    throw Exception(_T("Invalid buffer size for color cursor."));
  }
  memcpy(newCursorShape.getPixels()->getBuffer(), &buffer.front(), shapeSize);
  int maskPitch = ((dim.width + 15) / 16 ) * 2;
  std::vector<char> mask(maskPitch * dim.height, 0x00);
  bool maskedColor = shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
  WinCursorShapeUtils::winColorShapeToRfb<UINT32>(newCursorShape.getPixels(), &mask.front(), maskPitch);
  WinCursorShapeUtils::fixAlphaChannel(newCursorShape.getPixels(), &mask.front(), maskedColor, maskPitch);
  newCursorShape.assignMaskFromWindows(&mask.front()); // assumes width is aligned to 2 bytes
  cursorShape->clone(&newCursorShape);
}

