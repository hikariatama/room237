<img width="200" src="https://github.com/user-attachments/assets/d6771462-a81d-4160-b240-ea97e156471f" />

![open source](https://img.shields.io/badge/open%20source-545454)
[![GitHub Release](https://img.shields.io/github/v/release/hikariatama/room237)](github.com/hikariatama/room237/releases/latest)

> Lightweight offline photo management tool for those who need absolute privacy.

<img width="2624" height="1506" alt="room237-screen" src="https://github.com/user-attachments/assets/0a626deb-6cfb-42f8-b638-e63539c11865" />

> [!WARNING]
> ⚠️ Disclamier: Project is provided "as is" without any warranty. Use at your own risk. It is recommended to **keep a backup** of your photos before using this app.

<div align=left>
<table>
  <thead align=left>
    <tr>
      <th>OS</th>
      <th>Download</th>
    </tr>
  </thead>
  <tbody align="left">
    <tr>
      <td>Windows</td>
      <td>
        <a href="https://github.com/hikariatama/room237/releases/latest/download/room237-release-windows-x64-setup.exe">
          <img src="https://img.shields.io/badge/Setup-x64-67b7d1.svg?logo=windows">
        </a><br>
        <a href="https://github.com/hikariatama/room237/releases/latest/download/room237-release-windows-x64.msi">
          <img src="https://img.shields.io/badge/MSI-x64-2d7d9a.svg?logo=windows">
        </a><br>
        <a href="https://github.com/hikariatama/room237/releases/latest/download/room237-release-windows-x64.exe">
          <img src="https://img.shields.io/badge/Portable%20EXE-x64-3a8bd6.svg?logo=windows">
        </a>
      </td>
    </tr>
    <tr>
      <td>macOS</td>
      <td>
        <a href="https://github.com/hikariatama/room237/releases/latest/download/room237-release-darwin-x64.dmg">
          <img src="https://img.shields.io/badge/DMG-Intel%20x64-D33A54.svg?logo=apple">
        </a><br>
        <a href="https://github.com/hikariatama/room237/releases/latest/download/room237-release-darwin-aarch64.dmg">
          <img src="https://img.shields.io/badge/DMG-ARM64-D33A54.svg?logo=apple">
        </a><br>
        <a
          href="https://github.com/hikariatama/room237/releases/latest/download/room237-release-darwin-aarch64.app.tar.gz">
          <img src="https://img.shields.io/badge/.TAR.GZ-ARM64-D33A54.svg?logo=apple">
        </a><br>
        <a href="https://github.com/hikariatama/room237/releases/latest/download/room237-release-darwin-x64.app.tar.gz">
          <img src="https://img.shields.io/badge/.TAR.GZ-Intel%20x64-D33A54.svg?logo=apple">
        </a>
      </td>
    </tr>
    <tr>
      <td>Linux</td>
      <td>
        <a href="https://github.com/hikariatama/room237/releases/latest/download/room237-release-linux-amd64.AppImage">
          <img src="https://img.shields.io/badge/AppImage-x64-f84e29.svg?logo=linux">
        </a><br>
        <a
          href="https://github.com/hikariatama/room237/releases/latest/download/room237-release-linux-aarch64.AppImage">
          <img src="https://img.shields.io/badge/AppImage-ARM64-f84e29.svg?logo=linux">
        </a><br>
        <a href="https://github.com/hikariatama/room237/releases/latest/download/room237-release-linux-amd64.deb">
          <img src="https://img.shields.io/badge/DebPackage-x64-FF9966.svg?logo=debian">
        </a><br>
        <a href="https://github.com/hikariatama/room237/releases/latest/download/room237-release-linux-arm64.deb">
          <img src="https://img.shields.io/badge/DebPackage-ARM64-FF9966.svg?logo=debian">
        </a><br>
        <a href="https://github.com/hikariatama/room237/releases/latest/download/room237-release-linux-x86_64.rpm">
          <img src="https://img.shields.io/badge/RpmPackage-x64-F1B42F.svg?logo=redhat">
        </a><br>
        <a href="https://github.com/hikariatama/room237/releases/latest/download/room237-release-linux-aarch64.rpm">
          <img src="https://img.shields.io/badge/RpmPackage-ARM64-F1B42F.svg?logo=redhat">
        </a><br>
        <a href="https://github.com/hikariatama/room237/releases/latest/download/room237-release-linux-x86_64">
          <img src="https://img.shields.io/badge/Binary-x86__64-111111.svg?logo=linux">
        </a><br>
        <a href="https://github.com/hikariatama/room237/releases/latest/download/room237-release-linux-aarch64">
          <img src="https://img.shields.io/badge/Binary-aarch64-111111.svg?logo=linux">
        </a>
      </td>
    </tr>
  </tbody>
</table>

## What's that

> TL;DR: Offline-only photo manager, double-sided sync with folder of subfolders of photos.

Many people store their photos in a folder on their computer. Some use subfolders to categorize them, e.g. by years. Most of those people are either not familiar with cloud-sync services, or do not trust them for obvious reasons. Conventional open-source photo managers are complicated to set up, they roll their own storage system with MinIO, and are pretty much an overkill for a family photo storage. Not to mention that migrating away from them is a pain.

Room237 is aimed to solve all of these problems. You can open a folder with folders, wait a few minutes (depending on the size of the library) and use it. Do not like the UI/UX? Just close the app and delete it - your photos are still in the same folder as they were.

What the app does:

1. Creates meta directories, `.room237-metadata` and `.room237-thumb`. Metadata includes info about photos (date taken, hash, etc.), thumbnails are several KB in size and are used to ensure smooth performance on big libraries.
2. Converts all HEIC photos into JPG. This is the only destructive action made by the app. It is done because some (or all) web engines do not support displaying HEIC.

All actions are done by the embedded ffmpeg binary.

## Usage recommendations

### Lightweight usage

If you are not storing any sensitive photos and/or you do not know how to setup encrypted containers, you can just create a new folder and use it as root for the app.

### Full privacy

To ensure the best privacy and performance, follow these recommendations:

**MacOS**:

1. Create an encrypted disk image on your **primary** drive ([Create a secure disk image](https://support.apple.com/et-ee/guide/disk-utility/dskutl11888/mac))
2. Use it (or any subdirectory on it) as a root for Room 237

**Windows**:

1. ⚠️ Do **NOT** use BitLocker, PLEASE.
2. Use [VeraCrypt](https://www.veracrypt.fr/en/Home.html) to create an encrypted container
3. Use it (or any subdirectory on it) as a root for Room 237

**Linux**:

1. Use [VeraCrypt](https://www.veracrypt.fr/en/Home.html) to create an encrypted container
2. Use it (or any subdirectory on it) as a root for Room 237

## Privacy Policy

I do **NOT** collect anything. App works completely offline. All network requests stay within your machine, except for the ones that go to `github.com` to check for updates. You will get a popup when new update is available, before it is installed.

## Terms of Service

I do not accept contributions (sorry).  
I do accept issues.
