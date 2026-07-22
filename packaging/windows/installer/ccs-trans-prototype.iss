#ifndef StagingRoot
  #error StagingRoot must identify a verified Qt GUI staging directory
#endif
#ifndef OutputRoot
  #define OutputRoot "."
#endif
#ifndef AppVersion
  #define AppVersion "0.8.0"
#endif

[Setup]
AppId=ccs-trans-0.8-prototype
AppName=ccs-trans
AppVersion={#AppVersion}
AppPublisher=ccs-trans
DefaultDirName={localappdata}\Programs\ccs-trans
DefaultGroupName=ccs-trans
OutputDir={#OutputRoot}
OutputBaseFilename=ccs-trans-{#AppVersion}-Windows-x64-setup-prototype
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=auto
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\ccs-trans-gui.exe
ChangesEnvironment=no

[Files]
Source: "{#StagingRoot}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{userprograms}\ccs-trans"; Filename: "{app}\ccs-trans-gui.exe"

[Run]
Filename: "{app}\ccs-trans-gui.exe"; Description: "Launch ccs-trans"; Flags: nowait postinstall skipifsilent unchecked
