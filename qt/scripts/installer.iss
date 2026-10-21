#ifndef PackageDir
  #error PackageDir must point to a deployed Release package
#endif
#ifndef OutputDir
  #error OutputDir is required
#endif
#ifndef SetupName
  #error SetupName is required
#endif

#define AppVersion GetStringFileInfo(PackageDir + "\WinSCPQt.exe", "ProductVersion")

[Setup]
AppId={{F97978CD-A6B7-4D3A-BD34-7E353FA8C899}
AppName=WinSCP Qt
AppVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\WinSCPQt
DefaultGroupName=WinSCP Qt
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename={#SetupName}
SetupIconFile=..\resources\winscpqt.ico
UninstallDisplayIcon={app}\WinSCPQt.exe
LicenseFile=..\LICENSE
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
CloseApplications=no

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Excludes: "portable.ini"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\WinSCP Qt"; Filename: "{app}\WinSCPQt.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\WinSCP Qt"; Filename: "{app}\WinSCPQt.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\WinSCPQt.exe"; Description: "{cm:LaunchProgram,WinSCP Qt}"; Flags: nowait postinstall skipifsilent
