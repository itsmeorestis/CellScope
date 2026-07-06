// Built-in default dock layout, captured from a curated cellscope.ini.
// Loaded via ImGui::LoadIniSettingsFromMemory() on first run / Reset Layout /
// layout-version bump. Contains only ImGui layout sections (no [CellScope][State]),
// so application settings are never touched when the layout is reset.
#pragma once

inline const char* kDefaultLayoutIni = R"INI(
[Window][##CellScopeHost]
Pos=0,0
Size=1920,1027
Collapsed=0

[Window][Control]
Pos=0,23
Size=391,1004
Collapsed=0
DockId=0x00000001,0

[Window][Spectrum]
Pos=393,23
Size=1527,201
Collapsed=0
DockId=0x00000009,0

[Window][Waterfall]
Pos=393,226
Size=1527,120
Collapsed=0
DockId=0x0000000B,0

[Window][Spectrum (B)]
Collapsed=0
DockId=0x0000000A

[Window][Waterfall (B)]
Collapsed=0
DockId=0x0000000C

[Window][LTE]
Pos=393,348
Size=1527,338
Collapsed=0
DockId=0x00000007,0

[Window][UEs]
Pos=393,688
Size=1527,339
Collapsed=0
DockId=0x00000008,0

[Window][Traffic]
Pos=393,688
Size=1527,339
Collapsed=0
DockId=0x00000008,1

[Window][Calls]
Pos=393,688
Size=1527,339
Collapsed=0
DockId=0x00000008,2

[Docking][Data]
DockSpace         ID=0x66BD7B3F Window=0x31B8B32C Pos=0,23 Size=1920,1004 Split=X
  DockNode        ID=0x00000001 Parent=0x66BD7B3F SizeRef=391,900 Selected=0xC6859269
  DockNode        ID=0x00000002 Parent=0x66BD7B3F SizeRef=1007,900 Split=Y
    DockNode      ID=0x00000003 Parent=0x00000002 SizeRef=1007,201 Split=X
      DockNode    ID=0x00000009 Parent=0x00000003 SizeRef=502,269 Selected=0x376897DD
      DockNode    ID=0x0000000A Parent=0x00000003 SizeRef=503,269
    DockNode      ID=0x00000004 Parent=0x00000002 SizeRef=1007,801 Split=Y
      DockNode    ID=0x00000005 Parent=0x00000004 SizeRef=1007,120 Split=X
        DockNode  ID=0x0000000B Parent=0x00000005 SizeRef=502,263 Selected=0xE6EEA1A4
        DockNode  ID=0x0000000C Parent=0x00000005 SizeRef=503,263
      DockNode    ID=0x00000006 Parent=0x00000004 SizeRef=1007,679 Split=Y
        DockNode  ID=0x00000007 Parent=0x00000006 SizeRef=1007,338 Selected=0x69574546
        DockNode  ID=0x00000008 Parent=0x00000006 SizeRef=1007,339 CentralNode=1 Selected=0xE395A9DD
)INI";
