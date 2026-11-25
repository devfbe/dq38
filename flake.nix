{
  description = "dq38: a bare-metal UEFI RAM test that tells you which data line (DQ) flips";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      inherit (pkgs) lib;
      version = "1.0.0";

      # The UEFI application: freestanding C, compiled straight to PE/COFF with clang and lld-link
      # against the gnu-efi headers (no gnu-efi runtime library is linked).
      dq38 = pkgs.stdenvNoCC.mkDerivation {
        pname = "dq38";
        inherit version;
        src = lib.fileset.toSource {
          root = ./.;
          fileset = ./dq38.c;
        };
        nativeBuildInputs = [ pkgs.llvmPackages.clang-unwrapped pkgs.lld ];
        buildPhase = ''
          runHook preBuild
          clang -target x86_64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone \
            -mno-stack-arg-probe -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
            -I${pkgs.gnu-efi}/include/efi -I${pkgs.gnu-efi}/include/efi/x86_64 \
            -c dq38.c -o dq38.o
          lld-link -subsystem:efi_application -nodefaultlib -entry:efi_main dq38.o -out:dq38.efi
          runHook postBuild
        '';
        installPhase = ''
          runHook preInstall
          install -Dm644 dq38.efi $out/dq38.efi
          runHook postInstall
        '';
        meta = {
          description = "Bare-metal UEFI RAM test that reports flips per data line (DQ)";
          license = lib.licenses.mit;
          platforms = [ "x86_64-linux" ];
        };
      };

      # A GPT disk image with one EFI system partition: \EFI\BOOT\BOOTX64.EFI is dq38, \dq38\run.cfg
      # its configuration. dd it onto a USB stick; the log is written back to the same stick.
      mkImage = { cfg ? ./run.cfg }: pkgs.runCommand "dq38-usb-${version}.img"
        {
          nativeBuildInputs = [ pkgs.dosfstools pkgs.mtools pkgs.util-linux ];
        } ''
        export SOURCE_DATE_EPOCH=${toString self.lastModified or 0}
        truncate -s 64M disk.img
        sfdisk --quiet disk.img <<EOF
        label: gpt
        label-id: D0380000-0000-4000-8000-000000000000
        start=2048, size=126976, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, uuid=D0380000-0000-4000-8000-000000000001, name="DQ38"
        EOF
        truncate -s 62M esp.img
        mkfs.vfat -F 32 -s 1 -n DQ38 -i D0380001 --invariant esp.img >/dev/null
        mmd -i esp.img ::/EFI ::/EFI/BOOT ::/dq38
        mcopy -i esp.img ${dq38}/dq38.efi ::/EFI/BOOT/BOOTX64.EFI
        mcopy -i esp.img ${cfg} ::/dq38/run.cfg
        dd if=esp.img of=disk.img bs=1M seek=1 conv=notrunc status=none
        cp disk.img $out
      '';
      image = mkImage { };

      # Boot the image in QEMU/OVMF with a fault injected on DQ38 and check that the log reports
      # the positive control, exactly that one hit on the right line, and a clean end of run.
      testCfg = pkgs.writeText "run.cfg" ''
        SECONDS=1
        READS=2
        PATTERNS=1
        RESET=1
        INJECT=38
      '';
      qemuCheck = pkgs.runCommand "dq38-qemu-check"
        {
          nativeBuildInputs = [ pkgs.qemu_test pkgs.mtools pkgs.gnugrep ];
        } ''
        cp ${mkImage { cfg = testCfg; }} disk.img && chmod u+w disk.img
        cp ${pkgs.OVMF.fd}/FV/OVMF_VARS.fd vars.fd && chmod u+w vars.fd
        timeout 900 qemu-system-x86_64 -machine q35 -m 768 -smp 2 -nographic -no-reboot -net none \
          -drive if=pflash,format=raw,readonly=on,file=${pkgs.OVMF.fd}/FV/OVMF_CODE.fd \
          -drive if=pflash,format=raw,file=vars.fd \
          -drive file=disk.img,format=raw,if=virtio > console.txt 2>&1 || true
        mcopy -i disk.img@@1M ::/dq38/dq38.log dq38.log
        cat dq38.log
        fail() { echo "FAIL: $1"; exit 1; }
        grep -q '^positive control ok$' dq38.log || fail "positive control"
        grep -q '^HIT .* dq=38 lane=4 bit=6 beat=0 PERSISTENT$' dq38.log || fail "injected hit not reported on DQ38"
        [ "$(grep -c '^HIT ' dq38.log)" = 1 ] || fail "expected exactly one hit"
        grep -q '^SUMMARY .* hits=1 transient=0 persistent=1 ' dq38.log || fail "summary"
        grep -q '^  DQ38 (byte lane 4, bit 6): 1$' dq38.log || fail "per-line statistics"
        grep -q '^=== end$' dq38.log || fail "run did not finish"
        mkdir $out && cp dq38.log console.txt $out/
      '';
    in
    {
      packages.${system} = {
        inherit dq38 image;
        default = dq38;
      };

      checks.${system}.qemu = qemuCheck;

      formatter.${system} = pkgs.nixpkgs-fmt;
    };
}
