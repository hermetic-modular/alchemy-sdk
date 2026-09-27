/**
 * @file tests/host/firmware_tests.cpp
 * @brief FirmwareCatalog — the /alchemy listing the firmware picker draws.
 *
 * The production stack runs in-process: FirmwareCatalog → SdCard → real
 * ff.c → RAM diskio.  The flash half of the picker (QSPI erase / write /
 * verify, the reboot) needs the board and is not exercised here.
 */

#include "firmware_tests.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ff.h"

#include "alchemy/storage/firmware_catalog.h"
#include "alchemy/storage/sd_card.h"
#include "ram_diskio.h"

using namespace alchemy;

static int s_checks   = 0;
static int s_failures = 0;

#define WCHECK(cond)                                                       \
    do {                                                                   \
        s_checks++;                                                        \
        if (!(cond)) {                                                     \
            s_failures++;                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

namespace {

bool FormatRamDisk()
{
    ramdisk::Reset(131072u);   /* 64 MiB → FAT32 */
    static FATFS scratch;
    std::memset(&scratch, 0, sizeof(scratch));
    if (f_mount(&scratch, "0:", 0) != FR_OK) return false;
    std::vector<uint8_t> work(4096u);
    const FRESULT fr = f_mkfs("0:", FM_FAT32 | FM_SFD, 512u,
                              work.data(), static_cast<UINT>(work.size()));
    f_mount(nullptr, "0:", 0);
    return fr == FR_OK;
}

bool PutFile(const char* path, uint32_t bytes)
{
    static FIL f;
    std::memset(&f, 0, sizeof f);
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    std::vector<uint8_t> buf(bytes, 0xA5u);
    UINT w = 0;
    const bool ok = f_write(&f, buf.data(), static_cast<UINT>(bytes), &w) == FR_OK && w == bytes;
    f_close(&f);
    return ok;
}

void TestNoFolder()
{
    WCHECK(FormatRamDisk());
    SdCard          sd;
    FirmwareCatalog cat;
    sd.Init();
    cat.Init(sd);
    WCHECK(cat.State() == FirmwareCatalog::Status::NotScanned);
    WCHECK(cat.Scan(1000u) == FirmwareCatalog::Status::NoFolder);
    WCHECK(cat.Count() == 0u);
}

void TestEmptyAndFiltered()
{
    WCHECK(FormatRamDisk());
    SdCard          sd;
    FirmwareCatalog cat;
    sd.Init();
    cat.Init(sd);
    WCHECK(sd.EnsureMounted(1000u));
    WCHECK(f_mkdir("0:/alchemy") == FR_OK);
    WCHECK(cat.Scan(1000u) == FirmwareCatalog::Status::Empty);

    /* Nothing here is an image: AppleDouble twins, other extensions,
     * a directory named like a binary, a name too long to list. */
    WCHECK(PutFile("0:/alchemy/._mark.bin", 4096u));
    WCHECK(PutFile("0:/alchemy/notes.txt", 100u));
    WCHECK(PutFile("0:/alchemy/mark.hex", 4096u));
    WCHECK(f_mkdir("0:/alchemy/old.bin") == FR_OK);
    WCHECK(PutFile("0:/alchemy/abcdefghijklmnopqrstuvwxyz01.bin", 4096u));
    WCHECK(cat.Scan(1000u) == FirmwareCatalog::Status::Empty);
}

void TestSortedCaseInsensitive()
{
    WCHECK(FormatRamDisk());
    SdCard          sd;
    FirmwareCatalog cat;
    sd.Init();
    cat.Init(sd);
    WCHECK(sd.EnsureMounted(1000u));
    WCHECK(f_mkdir("0:/alchemy") == FR_OK);
    WCHECK(PutFile("0:/alchemy/smack_alchemy.bin", 3000u));
    WCHECK(PutFile("0:/alchemy/Belt_alchemy.bin", 2000u));
    WCHECK(PutFile("0:/alchemy/mark_alchemy.BIN", 1000u));
    WCHECK(PutFile("0:/alchemy/._Belt_alchemy.bin", 4096u));

    WCHECK(cat.Scan(1000u) == FirmwareCatalog::Status::Listed);
    WCHECK(cat.Count() == 3u);
    WCHECK(std::strcmp(cat.Name(0), "Belt_alchemy.bin") == 0);
    WCHECK(std::strcmp(cat.Name(1), "mark_alchemy.BIN") == 0);
    WCHECK(std::strcmp(cat.Name(2), "smack_alchemy.bin") == 0);
    WCHECK(cat.Size(0) == 2000u && cat.Size(1) == 1000u && cat.Size(2) == 3000u);

    char path[64];
    WCHECK(cat.Path(1, path, sizeof path));
    WCHECK(std::strcmp(path, "0:/alchemy/mark_alchemy.BIN") == 0);
    WCHECK(!cat.Path(3, path, sizeof path));   /* out of range */
    char tiny[8];
    WCHECK(!cat.Path(1, tiny, sizeof tiny));   /* would truncate */
    WCHECK(std::strcmp(cat.Name(7), "") == 0);
}

void TestCapAtSixteen()
{
    WCHECK(FormatRamDisk());
    SdCard          sd;
    FirmwareCatalog cat;
    sd.Init();
    cat.Init(sd);
    WCHECK(sd.EnsureMounted(1000u));
    WCHECK(f_mkdir("0:/alchemy") == FR_OK);
    char path[48];
    for (int i = 0; i < 20; i++)
    {
        std::snprintf(path, sizeof path, "0:/alchemy/fw%02d.bin", i);
        WCHECK(PutFile(path, 512u));
    }
    WCHECK(cat.Scan(1000u) == FirmwareCatalog::Status::Listed);
    WCHECK(cat.Count() == FirmwareCatalog::kMaxFiles);
}

void TestNoCard()
{
    WCHECK(FormatRamDisk());
    SdCard          sd;
    FirmwareCatalog cat;
    sd.Init();
    cat.Init(sd);
    ramdisk::FailAll(true);
    WCHECK(cat.Scan(1000u) == FirmwareCatalog::Status::NoCard);
    WCHECK(cat.Count() == 0u);
    ramdisk::ClearFaults();
    /* EnsureMounted's backoff holds the retry off until it has passed. */
    WCHECK(cat.Scan(1000u + SdCard::kRemountBackoffMs + 1u)
           == FirmwareCatalog::Status::NoFolder);
}

} // namespace

int RunFirmwareTests(int& checks, int& failures)
{
    TestNoFolder();
    TestEmptyAndFiltered();
    TestSortedCaseInsensitive();
    TestCapAtSixteen();
    TestNoCard();

    checks   += s_checks;
    failures += s_failures;
    return s_failures;
}
