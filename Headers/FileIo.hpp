class FileBase {
protected:
	std::wstring Path;
	uint8_t* FileData;
	uint32_t FileSize;
	bool Phantom;
public:
	virtual ~FileBase();
	FileBase(std::wstring TargetPath, bool bMemStore, bool bForceOpen = true);
	FileBase(std::wstring DesiredPath, const uint8_t* DataBuf, uint32_t dwSize);
	const uint8_t* GetData() const { return this->FileData; }
	uint32_t GetSize() const { return this->FileSize; }
	bool ToDisk(bool bAppend = false) const;
	std::wstring GetPath() const { return this->Path; }
	bool IsPhantom() const { return this->Phantom; }
	static bool TranslateDevicePath(const wchar_t* DevicePath, wchar_t* TranslatedPath);
	static bool ArchWow64PathExpand(const wchar_t* TargetFilePath, wchar_t* OutputPath, size_t ccOutputPathLength);
};