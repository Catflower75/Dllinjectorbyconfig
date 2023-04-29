#include <iostream>
#include <string>
#include <Windows.h>
#include <TlHelp32.h>
#include <locale>
#include <codecvt>
#include <vector>
#include <map>
#include <fstream>
#include <curl/curl.h>
#include <mmsystem.h>
#include <Shellapi.h>
#include "INIReader.h"

#pragma comment(lib, "winmm.lib")

DWORD FindProcessId(const std::wstring& processName)
{
    PROCESSENTRY32 processInfo;
    processInfo.dwSize = sizeof(processInfo);

    HANDLE processesSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
    if (processesSnapshot == INVALID_HANDLE_VALUE)
        return 0;

    Process32First(processesSnapshot, &processInfo);
    do
    {
        if (!_wcsicmp(processInfo.szExeFile, processName.c_str()))
        {
            CloseHandle(processesSnapshot);
            return processInfo.th32ProcessID;
        }
    } while (Process32Next(processesSnapshot, &processInfo));

    CloseHandle(processesSnapshot);
    return 0;
}

bool InjectDll(DWORD processId, const std::wstring& dllPath)
{
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, processId);
    if (!process)
        return false;

    LPVOID loadLibraryAddress = (LPVOID)GetProcAddress(GetModuleHandle(L"kernel32.dll"), "LoadLibraryW");
    if (!loadLibraryAddress)
        return false;

    LPVOID dllPathAddressInRemoteMemory = VirtualAllocEx(process, NULL, dllPath.size() * sizeof(wchar_t) + 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!dllPathAddressInRemoteMemory)
        return false;

    if (!WriteProcessMemory(process, dllPathAddressInRemoteMemory, dllPath.c_str(), dllPath.size() * sizeof(wchar_t) + 1, NULL))
        return false;

    HANDLE thread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)loadLibraryAddress, dllPathAddressInRemoteMemory, 0, NULL);
    if (!thread)
        return false;

    CloseHandle(thread);
    CloseHandle(process);

    return true;
}

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

bool DownloadFile(const std::string& url, const std::wstring& filePath)
{
    CURL* curl;
    CURLcode res;
    std::string buffer;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return false;
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();

    std::ofstream outFile(filePath, std::ios::binary);
    if (!outFile) {
        return false;
    }

    outFile.write(buffer.data(), buffer.size());
    outFile.close();

    return true;
}

void AfficherApps(const std::vector<std::wstring>& appNames) {
    std::wcout << L"Applications disponibles:" << std::endl;
    for (size_t i = 0; i < appNames.size(); ++i) {
        std::wcout << i + 1 << L". " << appNames[i] << std::endl;
    }
}

int main() {
    INIReader reader("config.ini");

    if (reader.ParseError() < 0) {
        std::cout << "Can't load 'config.ini'\n";
        return 1;
    }

    // Lire l'option autoStartApp1 de config.ini
    int autoStartApp1 = reader.GetInteger("", "autoStartApp1", 0);
    std::vector<std::wstring> appNames;
    std::map<std::wstring, std::tuple<std::wstring, std::string, std::string, std::string>> appToDllMap;

    // Charger les noms d'applications et les DLLs
    for (int i = 1; ; ++i) {
        std::string section = "app" + std::to_string(i);
        std::string appName_narrow = reader.Get(section, "name", "");
        if (appName_narrow.empty()) {
            break;
        }
        std::wstring appName(appName_narrow.begin(), appName_narrow.end());
        appNames.push_back(appName);

        std::string dllPath_narrow = reader.Get(section, "dllPath", "");
        std::wstring dllPath(dllPath_narrow.begin(), dllPath_narrow.end());
        std::string dllUrl = reader.Get(section, "dllUrl", "");
        std::string startupMethod = reader.Get(section, "startupMethod", "normal");
        std::string steamAppUri = reader.Get(section, "steamAppUri", "");

        appToDllMap[appName] = std::make_tuple(dllPath, dllUrl, startupMethod, steamAppUri);
    }


    size_t appChoice;

    // Vérifier si l'option autoStartApp1 est définie sur 1
    if (autoStartApp1 == 1) {
        appChoice = 1;
    }
    else {
        // Afficher les applications disponibles
        AfficherApps(appNames);

        // Obtenir l'entrée de l'utilisateur pour l'application choisie
        std::wcout << L"Entrez le numéro de l'application dans laquelle vous souhaitez injecter la DLL: ";
        std::cin >> appChoice;
    }

    if (appChoice < 1 || appChoice > appNames.size()) {
        std::wcout << L"Choix invalide. Sortie..." << std::endl;
        return 1;
    }

    std::wstring targetProcessName = appNames[appChoice - 1];
    std::wstring dllPath;
    std::string dllUrl;
    std::string startupMethod;
    std::string steamAppUri;

    std::tie(dllPath, dllUrl, startupMethod, steamAppUri) = appToDllMap[targetProcessName];

    // Télécharger la DLL
    if (!DownloadFile(dllUrl, dllPath)) {
        std::wcout << L"Échec du téléchargement de la DLL. Sortie..." << std::endl;
        return 1;
    }

    STARTUPINFO startupInfo = { 0 };
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo;

    // Démarrer l'application cible
    if (startupMethod == "steam" && !steamAppUri.empty()) {
        ShellExecute(NULL, L"open", std::wstring(steamAppUri.begin(), steamAppUri.end()).c_str(), NULL, NULL, SW_SHOWNORMAL);
        Sleep(10000); // Attendre que l'application démarre
    }
    else {
        if (!CreateProcess(targetProcessName.c_str(), NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &startupInfo, &processInfo)) {
            std::wcout << L"Échec de la création du processus. Sortie..." << std::endl;
            return 1;
        }
    }

    DWORD processId = startupMethod == "steam" ? FindProcessId(targetProcessName) : processInfo.dwProcessId;

    // Attendre 12 secondes avant d'injecter la DLL
    Sleep(12000);

    // Injecter la DLL
    if (!InjectDll(processId, dllPath)) {
        std::wcout << L"Échec de l'injection de la DLL. Sortie..." << std::endl;
        return 1;
    }

    if (startupMethod != "steam") {
        ResumeThread(processInfo.hThread);
    }

    std::wcout << L"Injection réussie !" << std::endl;

    // Jouer le son de confirmation
    PlaySound(L"SystemAsterisk", NULL, SND_ALIAS | SND_ASYNC);

    return 0;
}
