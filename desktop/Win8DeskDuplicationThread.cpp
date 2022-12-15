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

#include "WinDxRecoverableException.h"
#include "WinDxCriticalException.h"
#include "thread/AutoLock.h"

#include "WinDxgiAcquiredFrame.h"
#include "WinD3D11Texture2D.h"
#include "WinAutoMapDxgiSurface.h"

#include "Win8DeskDuplicationThread.h"

Win8DeskDuplication::Win8DeskDuplication(FrameBuffer *targetFb,
                                                     std::vector<Rect> &targetRect,
                                                     Win8CursorShape *targetCurShape,
                                                     LONGLONG *cursorTimeStamp,
                                                     LocalMutex *cursorMutex,
                                                     Win8DuplicationListener *duplListener,
                                                     std::vector<WinDxgiOutput> &dxgiOutput,
                                                     LogWriter *log)
: m_targetFb(targetFb),
  m_targetRects(targetRect),
  m_targetCurShape(targetCurShape),
  m_cursorTimeStamp(cursorTimeStamp),
  m_cursorMutex(cursorMutex),
  m_duplListener(duplListener),
  m_device(log),
  m_terminated(false),
  m_log(log)
{
  m_log->debug(_T("Creating Win8DeskDuplication for %d outputs"), dxgiOutput.size());
  for (size_t i = 0; i < dxgiOutput.size(); i++) {
    m_dxgiOutput1.push_back(&dxgiOutput[i]);
    m_outDupl.push_back(WinDxgiOutputDuplication(&m_dxgiOutput1[i], &m_device));
    m_rotations.push_back(dxgiOutput[i].getRotation());
    m_stageTextures2D.push_back(WinCustomD3D11Texture2D(m_device.getDevice(),
      (UINT)targetRect[i].getWidth(),
      (UINT)targetRect[i].getHeight(),
      m_rotations[i]));
  }
  m_log->debug(_T("Win8DeskDuplication created"));
}

Win8DeskDuplication::~Win8DeskDuplication()
{
  m_log->debug(_T("deleting Win8DeskDuplication"));
}

void Win8DeskDuplication::execute()
{
  const int ACQUIRE_TIMEOUT = 20;
  try {
    std::vector<int> timeouts;
    std::vector<DateTime> begins;
    timeouts.resize(m_outDupl.size());
    begins.resize(m_outDupl.size());
    while (!m_terminated) {
      for (size_t i = 0; i < m_outDupl.size(); i++) {
        {
          begins[i] = DateTime::now();
          WinDxgiAcquiredFrame acquiredFrame(&m_outDupl[i], ACQUIRE_TIMEOUT);
		      if (acquiredFrame.wasTimeOut()) {
			      timeouts[i]++;
			      m_log->debug(_T("Timeout on acquire frame for output: %d"), i);
			      Thread::yield();
			      continue;
          }
          int accum_frames = acquiredFrame.getFrameInfo()->AccumulatedFrames;
          double dt = (double)(DateTime::now() - begins[i]).getTime(); // in milliseconds

          m_log->debug(_T("Acquire frame for output: %d for %f ms, accumulated %d frames"), i, dt + ACQUIRE_TIMEOUT * timeouts[i], accum_frames);
          timeouts[i] = 0;
          WinD3D11Texture2D acquiredDesktopImage(acquiredFrame.getDxgiResource());
          DXGI_OUTDUPL_FRAME_INFO *info = acquiredFrame.getFrameInfo();

          // Get metadata
          if (info->TotalMetadataBufferSize) {
            size_t moveCount = m_outDupl[i].getFrameMoveRects(&m_moveRects);
            size_t dirtyCount = m_outDupl[i].getFrameDirtyRects(&m_dirtyRects);

            processMoveRects(moveCount, i);
            processDirtyRects(dirtyCount, &acquiredDesktopImage, i);
          }

          // Check cursor pointer for updates.
          try {
            processCursor(info, i);
			    } catch (WinDxException &e) {
		        m_log->debug(_T("Error on cursor processing: %s, (%x)"), e.getMessage(), (int)e.getErrorCode());
			    } // Cursor
        }
        Thread::yield();
      }
    }
  } catch (WinDxRecoverableException &e) {
    StringStorage errMess;
    errMess.format(_T("Win8DeskDuplication:: Catched WinDxRecoverableException: %s, (%x)"), e.getMessage(), (int)e.getErrorCode());
    m_duplListener->onRecoverableError(errMess.getString());
  } catch (WinDxCriticalException &e) {
    StringStorage errMess;
    errMess.format(_T("Win8DeskDuplication:: Catched WinDxCriticalException: %s, (%x)"), e.getMessage(), (int)e.getErrorCode());
    m_duplListener->onCriticalError(errMess.getString());
  } catch (Exception &e) {
    StringStorage errMess;
    errMess.format(_T("Win8DeskDuplication:: Catched WinDxCriticalException: %s") , e.getMessage());
    m_duplListener->onCriticalError(errMess.getString());
  }
}

void Win8DeskDuplication::terminate()
{
  m_terminated = true;
}

Dimension Win8DeskDuplication::getStageDimension(size_t out) const
{
  return Dimension(m_stageTextures2D[out].getDesc()->Width, m_stageTextures2D[out].getDesc()->Height);
}

void Win8DeskDuplication::processMoveRects(size_t moveCount, size_t out)
{
  _ASSERT(moveCount <= m_moveRects.size());
  Rect destinationRect;
  Rect sourceRect;
  Rect targetRect = m_targetRects[out];
  DXGI_MODE_ROTATION rotation = m_rotations[out];

  for (size_t iRect = 0; iRect < moveCount; iRect++) {
    destinationRect.fromWindowsRect(&m_moveRects[iRect].DestinationRect);
    sourceRect = destinationRect;
    POINT srcPoint = m_moveRects[iRect].SourcePoint;
    sourceRect.setLocation(srcPoint.x, srcPoint.y);
    rotateRectInsideStage(&destinationRect, &getStageDimension(out), rotation);
    rotateRectInsideStage(&sourceRect, &getStageDimension(out), rotation);
    // Translate the rect and point to the frame buffer coordinates.
    destinationRect.move(targetRect.left, targetRect.top);
    sourceRect.move(targetRect.left, targetRect.top);
    int x = sourceRect.left;
    int y = sourceRect.top;
    m_targetFb->move(&destinationRect, x, y);

    m_duplListener->onCopyRect(&destinationRect, x, y);
  }
}

void Win8DeskDuplication::processDirtyRects(size_t dirtyCount,
                                                  WinD3D11Texture2D *acquiredDesktopImage, 
                                                  size_t out)
{
  _ASSERT(dirtyCount <= m_dirtyRects.size());

  Region changedRegion;

  Rect dirtyRect;
  Dimension stageDim = getStageDimension(out);
  Rect stageRect = stageDim.getRect();

  DXGI_MODE_ROTATION rotation = m_rotations[out];

  for (size_t iRect = 0; iRect < dirtyCount; iRect++) {
    dirtyRect.fromWindowsRect(&m_dirtyRects[iRect]);

    if (!stageRect.isFullyContainRect(&dirtyRect)) {
      dirtyRect = dirtyRect.intersection(&stageRect);
      /* Disabled the followed throwing because it realy may happen and better is to see any picture
      // instead of a black screen.
      StringStorage errMess;
      errMess.format(_T("During processDirtyRects has been got a rect (%d, %d, %dx%d) which outside")
                     _T(" from the stage rect (%d, %d, %dx%d)"),
                     rect.left, rect.top, rect.getWidth(), rect.getHeight(),
                     stageRect.left, stageRect.top, stageRect.getWidth(), stageRect.getHeight());
      throw Exception(errMess.getString());
      */
    }
    ID3D11Texture2D *texture = m_stageTextures2D[out].getTexture();
    m_device.copySubresourceRegion(texture, dirtyRect.left, dirtyRect.top,
      acquiredDesktopImage->getTexture(), &dirtyRect, 0, 1);

    WinDxgiSurface surface(texture);
    WinAutoMapDxgiSurface autoMapSurface(&surface, DXGI_MAP_READ);

    Rect dstRect(dirtyRect);
    rotateRectInsideStage(&dstRect, &stageDim, rotation);
    // Translate the rect to the frame buffer coordinates.
    dstRect.move(m_targetRects[out].left, m_targetRects[out].top);
    m_log->debug(_T("Destination dirty rect = %d, %d, %dx%d"), dstRect.left, dstRect.top, dstRect.getWidth(), dstRect.getHeight());

    stageDim.width = static_cast<int> (autoMapSurface.getStride() / 4);
    m_auxiliaryFrameBuffer.setPropertiesWithoutResize(&stageDim, &m_targetFb->getPixelFormat());
    m_auxiliaryFrameBuffer.setBuffer(autoMapSurface.getBuffer());
    switch (rotation)
    {
      case DXGI_MODE_ROTATION_UNSPECIFIED:
      case DXGI_MODE_ROTATION_IDENTITY:
      {
        m_targetFb->copyFrom(&dstRect, &m_auxiliaryFrameBuffer, dirtyRect.left, dirtyRect.top);
        break;
      }
      case DXGI_MODE_ROTATION_ROTATE90:
      {
        m_targetFb->copyFromRotated90(&dstRect, &m_auxiliaryFrameBuffer, dirtyRect.left, dirtyRect.top);
        break;
      }
      case DXGI_MODE_ROTATION_ROTATE180:
      {
        m_targetFb->copyFromRotated180(&dstRect, &m_auxiliaryFrameBuffer, dirtyRect.left, dirtyRect.top);
        break;
      }
      case DXGI_MODE_ROTATION_ROTATE270:
      {
        m_targetFb->copyFromRotated270(&dstRect, &m_auxiliaryFrameBuffer, dirtyRect.left, dirtyRect.top);
        break;
      }
    }
    m_auxiliaryFrameBuffer.setBuffer(0);

    changedRegion.addRect(&dstRect);
  }

  m_duplListener->onFrameBufferUpdate(&changedRegion);
}

void Win8DeskDuplication::rotateRectInsideStage(Rect *toTranspose,
                                                      const Dimension *stageDim,
                                                      DXGI_MODE_ROTATION rotation)
{
  int left = toTranspose->left;
  int top = toTranspose->top;
  int width = toTranspose->getWidth();
  int height = toTranspose->getHeight();
  switch (rotation)
  {
    case DXGI_MODE_ROTATION_UNSPECIFIED:
    case DXGI_MODE_ROTATION_IDENTITY:
    {
      return;
    }
    case DXGI_MODE_ROTATION_ROTATE90:
    {
      toTranspose->rotateOn90InsideDimension(stageDim->height);
      break;
    }
    case DXGI_MODE_ROTATION_ROTATE180:
    {
      toTranspose->rotateOn180InsideDimension(stageDim->width, stageDim->height);
      break;
    }
    case DXGI_MODE_ROTATION_ROTATE270:
    {
      toTranspose->rotateOn270InsideDimension(stageDim->width);
      break;
    }
  }
}

void Win8DeskDuplication::processCursor(const DXGI_OUTDUPL_FRAME_INFO *info, size_t out)
{
  AutoLock al(m_cursorMutex);
  LONGLONG lastUpdateTime = info->LastMouseUpdateTime.QuadPart;
  if (lastUpdateTime != 0 && lastUpdateTime > *m_cursorTimeStamp) {
    *m_cursorTimeStamp = lastUpdateTime;

    //
    DXGI_OUTDUPL_POINTER_POSITION pointerPos = info->PointerPosition;

    bool newVisibility = pointerPos.Visible != FALSE;
    bool visibleChanged = m_targetCurShape->getIsVisible() != newVisibility;
    if (visibleChanged) {
	  m_log->debug(newVisibility ? _T("Cursor became visible") : _T("Cursor became not visible"));
      m_targetCurShape->setVisibility(newVisibility, out);
      m_duplListener->onCursorShapeChanged();
    }

    //
    bool shapeChanged = info->PointerShapeBufferSize != 0;
    if (shapeChanged) {
	    m_log->debug(_T("Cursor shape chagned"));
	    m_outDupl[out].getFrameCursorShape(m_targetCurShape->getCursorShapeForWriting(), info->PointerShapeBufferSize, m_log);
      m_duplListener->onCursorShapeChanged();
    }

    if (pointerPos.Visible) {
  	  m_log->debug(_T("Cursor position chagned"));
      Point hotPoint = m_targetCurShape->getCursorShape()->getHotSpot();
      Rect targetRect = m_targetRects[out];
      m_duplListener->onCursorPositionChanged(pointerPos.Position.x + targetRect.left + hotPoint.x,
                                              pointerPos.Position.y + targetRect.top + hotPoint.y);
    }
  }
}
