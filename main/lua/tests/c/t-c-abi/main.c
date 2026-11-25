// An app built for a newer ABI than the badge's: refused at launch. Declared
// by hand: badge_abi.h's own badge_abi() (weak) can't be overridden in the
// same file.
__attribute__((import_module("badge"), import_name("log"))) void sos_log(const char *p, unsigned len);

__attribute__((export_name("badge_abi"))) int badge_abi(void)
{
    return 99;
}

__attribute__((export_name("on_start"))) void on_start(void)
{
    sos_log("must not start", 14);
}
