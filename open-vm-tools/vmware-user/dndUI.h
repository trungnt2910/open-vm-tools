/*********************************************************
 * Copyright (C) 2009 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/**
 * @file dndUI.h
 *
 *    Implement the methods that allow DnD between host and guest for
 *    protocols V3 or greater.
 *
 */

#ifndef DND_UI_H
#define DND_UI_H

#include "stringxx/string.hh"

extern "C" {
#include "debug.h"
#include "dnd.h"
#include "str.h"
#include "util.h"
#include "vmblock.h"
#include "dndClipboard.h"
#include "dynbuf.h"
#include "../dnd/dndFileContentsUtil.h"
#include "dynxdr.h"
#include "cpNameUtil.h"
#include "posix.h"
}

#include "dnd.hh"
#include "dndFileList.hh"
#include "dragDetWnd.h"

struct DblLnkLst_Links;

/**
 * The DnDUI class implements the UI portion of DnD V3 and greater
 * versions of the protocol.
 */
class DnDUI
{
public:
   DnDUI(DblLnkLst_Links *eventQueue);
   ~DnDUI();
   bool Init();
   void VmxDnDVersionChanged(struct RpcIn *rpcIn,
                             uint32 version)
      {ASSERT(m_DnD); m_DnD->VmxDnDVersionChanged(rpcIn, version);}
   void SetDnDAllowed(bool isDnDAllowed)
      {ASSERT(m_DnD); m_DnD->SetDnDAllowed(isDnDAllowed);}
   void SetBlockControl(DnDBlockControl *blockCtrl);
   void SetUnityMode(Bool mode)
      {m_unityMode = mode;};

   DragDetWnd *GetFullDetWnd() {return m_detWnd;}
   GtkWidget *GetDetWndAsWidget();

private:

   /**
    * Blocking FS Helper Functions.
    */
   void AddBlock();
   void RemoveBlock();

   /**
    * Callbacks from Common DnD layer.
    */
   void CommonResetCB();
   void CommonUpdateMouseCB(int32 x, int32 y);

   /**
    * Source functions for HG DnD.
    */
   void CommonDragStartCB(const CPClipboard *clip, std::string stagingDir);
   void CommonSourceDropCB(void);

   /**
    * Called when HG Dnd is completed.
    */
   void CommonSourceCancelCB(void);

   /**
    * Called when GH DnD is completed.
    */
   void CommonDestPrivateDropCB(int32 x, int32 y);
   void CommonDestCancelCB(void);

   /**
    * Source functions for file transfer.
    */
   void CommonSourceFileCopyDoneCB(bool success, std::vector<uint8> stagingDir);

   /**
    * Callbacks for showing/hiding detection window.
    */
   void CommonUpdateDetWndCB(bool bShow, int32 x, int32 y);
   void CommonUpdateUnityDetWndCB(bool bShow, uint32 unityWndId, bool bottom);
   void CommonMoveDetWndToMousePos(void);

   /**
    * Gtk+ Callbacks: Drag Destination.
    */
   void GtkDestDragDataReceivedCB(const Glib::RefPtr<Gdk::DragContext> &dc,
                                  int x, int y, const Gtk::SelectionData &sd,
                                  guint info, guint time);
   bool GtkDestDragDropCB(const Glib::RefPtr<Gdk::DragContext> &dc,
                          int x, int y, guint time);
   void GtkDestDragLeaveCB(const Glib::RefPtr<Gdk::DragContext> &dc,
                           guint time);
   bool GtkDestDragMotionCB(const Glib::RefPtr<Gdk::DragContext> &dc, int x,
                            int y, guint time);

   /**
    * Gtk+ Callbacks: Drag Source.
    */
   void GtkSourceDragBeginCB(const Glib::RefPtr<Gdk::DragContext>& context);
   void GtkSourceDragDataGetCB(const Glib::RefPtr<Gdk::DragContext>& context,
                               Gtk::SelectionData& selection_data, guint info,
                               guint time);
   void GtkSourceDragEndCB(const Glib::RefPtr<Gdk::DragContext>& context);
   /**
    * Source functions for HG DnD. Makes calls to common layer.
    */
   void SourceDragStartDone(void);
   void SourceUpdateFeedback(DND_DROPEFFECT effect);

   /**
    * Target function for GH DnD. Makes call to common layer.
    */
   void TargetDragEnter(void);

   /**
    * Misc methods.
    */
   bool SetCPClipboardFromGtk(const Gtk::SelectionData& sd);
   std::string GetLastDirName(const std::string &str);
   utf::utf8string GetNextPath(utf::utf8string &str, size_t& index);
   DND_DROPEFFECT ToDropEffect(Gdk::DragAction action);
   void SetTargetsAndCallbacks();
   bool SendFakeXEvents(const bool showWidget, const bool buttonEvent,
                        const bool buttonPress, const bool moveWindow,
                        const bool coordsProvided,
                        const int xCoord, const int yCoord);
   bool SendFakeMouseMove(const int x, const int y);
   bool WriteFileContentsToStagingDir();
   unsigned long GetTimeInMillis();

   DblLnkLst_Links *m_eventQueue;
   DnD *m_DnD;
   std::string m_HGStagingDir;
   utf::string m_HGFileContentsUriList;
   DragDetWnd *m_detWnd;
   CPClipboard m_clipboard;
   DnDBlockControl *m_blockCtrl;
   bool m_HGGetDataInProgress;
   int m_HGEffect;
   bool m_blockAdded;

   /* State to determine if drag motion is a drag enter. */
   bool m_GHDnDInProgress;
   /* Icon updates from the guest. */
   /* Only update mouse when we have clipboard contents from the host. */
   bool m_GHDnDDataReceived;
   bool m_GHDnDDropOccurred;
   bool m_unityMode;
   bool m_inHGDrag;
   DND_DROPEFFECT m_effect;
   bool m_isFileDnD;
   int32 m_mousePosX;
   int32 m_mousePosY;
   GdkDragContext *m_dc;
   unsigned long m_destDropTime;
};

#endif // DND_UI_H
