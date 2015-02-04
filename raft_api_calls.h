// -*- c++ -*-
// Warning: not for general use!

// align with C-u M-x align-regexp RET SPC RET RET RET y
// see http://www.emacswiki.org/emacs/AlignCommands

api_call(Apply,         ApplyArgs,    true)
api_call(Barrier,       BarrierArgs,  true)
api_call(VerifyLeader,  NoArgs,       false)
api_call(GetState,      NoArgs,       true)
api_call(Snapshot,      NoArgs,       false)
api_call(AddPeer,       NetworkAddr,  false)
api_call(RemovePeer,    NetworkAddr,  false)
api_call(Shutdown,      NoArgs,       false)
