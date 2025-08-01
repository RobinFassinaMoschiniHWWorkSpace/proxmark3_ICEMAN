#!/usr/bin/env python3
# MIT License
# Copyright (c) 2020 @doegox

# Requirements:
# python3 -m pip install ansicolors sslcrypto

import binascii
import sys
import sslcrypto
from colors import color

debug = False


def guess_curvename(signature):
    siglen = (len(signature) // 2) & 0xfe
    if siglen == 32:
        curves = ["secp128r1", "secp128r2"]
    elif siglen == 48:
        curves = ["secp192k1", "prime192v1"]
    elif siglen == 56:
        curves = ["secp224k1", "secp224r1"]
    elif siglen == 64:
        curves = ["secp256k1", "secp256r1"]
    elif siglen == 96:
        curves = ["secp384r1"]
    elif siglen == 132:
        curves = ["secp521r1"]
    else:
        lenstr = color('%i', fg='red') % len(signature)
        raise ValueError("Unsupported signature size %s" % lenstr)
    return curves


def recover(data, signature, curvename, alghash=None):
    recovered = set()
    try:
        curve = sslcrypto.ecc.get_curve(curvename)
    except ValueError:
        print("Warning, your OpenSSL doesn't provide support for curve", curvename)
        return recovered
    recoverable = len(signature) % 1 == 1
    if (recoverable):
        try:
            pk = curve.recover(signature, data, hash=alghash)
            recovered.add(pk)
            if debug:
                print("Possible Pk:    ", binascii.hexlify(pk))
        except ValueError:
            pass
    else:
        for i in range(2):
            # Brute force RECID
            recid = bytes([27+i])
            try:
                pk = curve.recover(recid + signature, data, hash=alghash)
                recovered.add(pk)
                if debug:
                    print("Possible Pk:    ", binascii.hexlify(pk))
            except ValueError:
                pass
    return recovered


def recover_multiple(uids, sigs, curvename, alghash=None):
    recovered = set()
    assert len(uids) == len(sigs)
    for i in range(len(uids)):
        data = binascii.unhexlify(uids[i])
        if debug:
            print("UID       (%2i): " %
                  len(data), binascii.hexlify(data))
        signature = binascii.unhexlify(sigs[i])
        if debug:
            print("Signature (%2i): " %
                  len(signature), binascii.hexlify(signature))
        recovered_tmp = recover(data, signature, curvename, alghash)
        if i == 0:
            if recovered_tmp == set():
                break
            else:
                recovered = recovered_tmp
        else:
            recovered &= recovered_tmp
    return recovered


def selftests():
    tests = [
        {'name': "Mifare Ultralight EV1",
         'samples': ["04C1285A373080", "CEA2EB0B3C95D0844A95B824A7553703B3702378033BF0987899DB70151A19E7",
                     "04C2285A373080", "A561506723D422D29ED9F93E60D20B9ED1E05CC1BF81DA19FE500CA0B81CC0ED"],
         'pk': "0490933BDCD6E99B4E255E3DA55389A827564E11718E017292FAF23226A96614B8"},

        {'name': "NTAG21x",
         'samples': ["04E10CDA993C80", "8B76052EE42F5567BEB53238B3E3F9950707C0DCC956B5C5EFCFDB709B2D82B3",
                     "04DB0BDA993C80", "6048EFD9417CD10F6B7F1818D471A7FE5B46868D2EABDC6307A1E0AAE139D8D0"],
         'pk': "04494E1A386D3D3CFE3DC10E5DE68A499B1C202DB5B132393E89ED19FE5BE8BC61"},

        {'name': "Mifare Classic EV1",
         'samples': ["0433619AB35780", "B9FAE369EC21C980650D87ED9AE9B1610E859131B4B8699C647548AB68D249BB",
                     "524374E2",       "F8758CE30A58553A9985C458FB9C7D340FCFB04847B928A0667939272BC58B5E",
                     "53424B8A",       "B4F533E8C06C021E242EFE8558C1672ED7022E5AE4E7AA2D46113B0AB6928AFC",
                     "BD2A4146",       "19505576ED327D8F8870C86B1ED00898BFEDFFF27CC82FC515BA2EEC26050873"],
         'pk': "044F6D3F294DEA5737F0F46FFEE88A356EED95695DD7E0C27A591E6F6F65962BAF"},

        {'name': "DESFire Light",
         'samples': ["0439556ACB6480", "D5BD0978106E1E38B513642335966AB21E9F950DCFCFAB45FF13D0DC3CA4C2AE7E0D671DF1240937D040DAC4601C5F66ED62C546EE03ED08",   # noqa: E501
                     "043B156ACB6480", "76B46932BF2FCF4931A24C755F5CB1686B914F1856177686B864BDAD58EFA6A7493E5C2232F3ADDAA434EA4647BFD1D385BDA6115E77D74C"],  # noqa: E501
         'pk': "040E98E117AAA36457F43173DC920A8757267F44CE4EC5ADD3C54075571AEBBF7B942A9774A1D94AD02572427E5AE0A2DD36591B1FB34FCF3D"},                        # noqa: E501

        {'name': "DESFire EV2",
         'samples': ["042A41CAE45380", "B2769F8DDB575AEA2A680ADCA8FFED4FAB81A1E9908E2B82FE0FABB697BBD9B23835C416970E75768F12902ACA491349E94E6589EAF4F508",   # noqa: E501
                     "045640CAE45380", "D34B53A8C2C100D700DEA1C4C0D0DE4409F3A418CD8D57C4F41F146E42AD9A55F014199ABBF5CA259C7799DB0AE20D5E77D4950AC7E95D33",   # noqa: E501
                     "040D259A965B80", "B158073A7100C88C3726F4299FA58311FC3CB18744686DE3F234928AD74578F5CAD7FCEC1DCB962ECC7CC000B8557B37F45B76DC6573A58F"],  # noqa: E501
         'pk': "04B304DC4C615F5326FE9383DDEC9AA892DF3A57FA7FFB3276192BC0EAA252ED45A865E3B093A3D0DCE5BE29E92F1392CE7DE321E3E5C52B3A"},                        # noqa: E501

        {'name': "DESFire EV2 XL",
         'samples': ["044ca092806480", "9d86dacd3866058b1cf122ff5fc80e997251d99179bc1f996acf6ed7d495da5c39dde699e2760c08d747ef08487b9897d48957e5afd755e2",   # noqa: E501
                     "045793d28a6380", "e509576a484b4f93b5b97ffa04cb297cae97cff1071bdefd23d5054513e3036203fdd1cdd2cdead0aead88df24ffe7cdaafee1e58a55a745",   # noqa: E501
                     "044ba492806480", "517b2931355bd9b9f35d72ed90bdab6212d05853abcf9dd45a79d5ceb91d8939c2c90d3a630a4d18a33903a3e23950a7580cf4ca34d03a90"],  # noqa: E501
         'pk': "04CD5D45E50B1502F0BA4656FF37669597E7E183251150F9574CC8DA56BF01C7ABE019E29FEA48F9CE22C3EA4029A765E1BC95A89543BAD1BC"},                        # noqa: E501

        {'name': "DESFire EV3",
         'samples': ["04448BD2DB6B80", "5CBB5632795C8F15263FEFB095B51C7B541AFD914A1AE44EF6FB8AF605EDF13DBFEE6C3A2DB372245E671DFE0D42CB1F0D0B8FE67A89D2F6",   # noqa: E501
                     "04445DD2DB6B80", "166BFD9F9BFAA451172566101580DF9894F582C4A4E258C15037AD2F35A475CF1D7FB817618623A6569F991931AFB2766984E21A18512A6D"],  # noqa: E501
         'pk': "041DB46C145D0A36539C6544BD6D9B0AA62FF91EC48CBC6ABAE36E0089A46F0D08C8A715EA40A63313B92E90DDC1730230E0458A33276FB743"},                        # noqa: E501

        {'name': "Mifare Plus EV1",
         'samples': ["042A2B221C5080", "BAC40CD88E9193C58ADA5055350C4F648EB5A7AEC4FCF9BD4CDD7B1C558DE5F59C6636F26286ED48622AAA2331D4DF1CEE23B57B94BDA631",   # noqa: E501
                     "04505082346B80", "78B2FCF6769F60B165F5BDEB3A6D0C26967BB165E65A3B400A01C711356FF0A0807AB1A2706FCA419702AC67211287E31D71927BA25AB235",   # noqa: E501
                     "12817C48",       "3351979A3449CACD9EE113A75B862917F03EFAE68DA399C06342BF8583C88DFE769DF49754A96F7C28B57189FB05B9C10E2305D41423A6EB"],  # noqa: E501
         'pk': "044409ADC42F91A8394066BA83D872FB1D16803734E911170412DDF8BAD1A4DADFD0416291AFE1C748253925DA39A5F39A1C557FFACD34C62E"},                        # noqa: E501

        {'name': "NTAG413DNA, DESFire EV1",
         'samples': ["042468222F5C80", "B9211E320F321BD1D0E158E10FF15109B389638BAE15D9909D7725BF1250ED236D66F1AF75C94D60330E4E92535F5E6997675281A5687173",   # noqa: E501
                     "042938222F5C80", "18B642797D1FD71806146A7A6EC778D3FDD04F39C4A3B36A592BD1A114DC44E5528380FA766C0B7EA32B284AFBE84300B620369F0686D8CC"],  # noqa: E501
         'pk': "04BB5D514F7050025C7D0F397310360EEC91EAF792E96FC7E0F496CB4E669D414F877B7B27901FE67C2E3B33CD39D1C797715189AC951C2ADD"},                        # noqa: E501

        {'name': "NTAG424DNA",
         'samples': ["0463474AA26A80", "27E9A50E6CA4BA9037C02F7D20A80D0284D0C1D83C67F5A5AC1D8A4EF86C9508417E4E9C6F85AA7920F0ABDED984CAF20467D66EA54BBF08",   # noqa: E501
                     "04C46C222A6380", "344A806EBF704C05C19215D2F840529CE365AAD2D08A469A95896D75D477D9FAB02A0C827E9F215BD8EB0E56A3A9A008FB75D706AABBD4DA"],  # noqa: E501
         'pk': "048A9B380AF2EE1B98DC417FECC263F8449C7625CECE82D9B916C992DA209D68422B81EC20B65A66B5102A61596AF3379200599316A00A1410"},                        # noqa: E501

        {'name': "Vivokey Spark1",
         # ! tag signature bytes output by pm3 must be read right to left:
         # echo $sig |sed 's/\(..\)/\1\n/g'|tac|tr -d '\n'
         # (and it uses a SHA256)
         'samples': ["E0040118009C870C", "4B4E03E1211952EF6A5F9D84AB218CD4D7549D0CDF8CA8779F9AD16C9A9CBF3B",
                     "E0040118009B4D62", "25CF13747C3389EC7889DE916E3747584978511CC78B51CFB1883B494CBED7AB"],
         'pk': "04D64BB732C0D214E7EC580736ACF847284B502C25C0F7F2FA86AACE1DADA4387A"},

        {'name': "ICODE DNA, ICODE SLIX2",
         # ! tag UID is considered inverted: E0040118009B5FEE => EE5F9B00180104E0
         # uses secp128r1, None,
         'samples': ["EE5F9B00180104E0", "32D9E7579CD77E6F1FA11419231E874826984C5F189FDE1421684563A9663377",
                     "838ED22A080104E0", "CAE8183CB4823C765AFDEB78C9D66C959990FD52A5820E76E1D6E025D76EAD79",
                     "1F62C26F080104E0", "9F47BF33D9ED62A62AFEF5685BC4006CFB290C344EAD5C5B49BF976804A9EE62"],
         'pk': "048878A2A2D3EEC336B4F261A082BD71F9BE11C4E2E896648B32EFA59CEA6E59F0"},
        # {'name': "Minecraft Earth",
        #  # uses secp256r1?, SHA-256,
        #  'samples': ["aa", "DF0E506DFF8FCFC4B7B979D917644445F1230D2C7CDC342AFA842CA240C210BE7275F62073A9670F2DCEFC602CBEE771C2B4CD4A04F3D1EA11F49ABDF7E8B721"], # noqa: E501
        #  'pk': ""},
        {'name': "MIFARE Plus Trojka",
         # uses secp224r1, None,
         'samples': ["04B59F6A226F82", "6F577EB7F570D74DB6250477427F68A0088762BD318767537122919A7916597149F9D16D8B135E9BF826FB28AE293F3168661CD4A049FAED",   # noqa: E501
                     "04B44A82D80F92", "A0868ECF26733D3C3C838D055968B4559F77693CC3E346E3A4741BC826801F8360FD88857BEC440AAD3A21153D64302DEB6F5ED40B15C3F7"],  # noqa: E501
         'pk': "040F732E0EA7DF2B38F791BF89425BF7DCDF3EE4D976669E3831F324FF15751BD52AFF1782F72FF2731EEAD5F63ABE7D126E03C856FFB942AF"},                        # noqa: E501

        {'name': "MIFARE Ultralight AES",
         # uses prime192v1, None,
         'samples': ["045E4CC2451390", "C9BBDA1B99EB6634CDFD8E3251AC5C4742EA5FA507B8A8A8B39B19AB7340D173331589C54C56C49F0CCA6DDBAC1E492A",                   # noqa: E501
                     "043F88C2451390", "5C2055A7373F119C3FDD9843020B06AA0E6DE18C16496C425C4AD971A50F05FA1A67B9E39CA60C355EEEEBF8214A84A5"],                  # noqa: E501
         'pk': "0453BF8C49B7BD9FE3207A91513B9C1D238ECAB07186B772104AB535F7D3AE63CF7C7F3DD0D169DA3E99E43C6399621A86"},                                        # noqa: E501
        {'name': "MIFARE Ultralight AES (alt key)",
         # uses prime192v1, None,
         # TODO more samples
         'samples': ["04A31232241C90", "057595DCC601CA7E21341F1F978FA134F0204D87A33749C56DDB4ABD6F1F26194341DB10093B34C42F524A30DCC5CE54"],                  # noqa: E501
         'pk': "04DC34DAA903F2726A6225B11C692AF6AB4396575CA12810CBBCE3F781A097B3833B50AB364A70D9C2B641A728A599AE74"},                                        # noqa: E501
        {'name': "MIFARE Classic / QL88",
         'samples': ["30933C61", "AEA4DD0B800FAC63D4DE08EE91F4650ED825FD6B4D7DEEE98DBC9BAE10BE003E",
                     "20593261", "F762CDD59EEDC075F4DDBA7ECD529FEEE5135C65A84D12EF0A250A321B2012F5"],
         'pk': "046F70AC557F5461CE5052C8E4A7838C11C7A236797E8A0730A101837C004039C2"},

        {'name': "NTAG223DNA, NTAG224DNA",
         # uses prime192v1, None,
         # TODO more samples
         'samples': ["043302218D3D00", "6A5D5E034F4FC823CACAB56C1A77A409B8DB345F89BD3FD59ED1F9C0093518609BE62D0A20764D2011E47EFA187F29AA"],                  # noqa: E501
         'pk': "0485D5B9353B4FAA77581BA2AE96630C5876D6E8603308ABE9A81A0B506F52D02D04FEE6F2D365B3DEE7B9FAD9133E2976"},                                        # noqa: E501

        # TruST25 (ST25TA) - KeyID 0x01?
        # curve=secp128r1, hash=sha256 - from block 224 in ST25TA NDEF file
        {'name': "ST25TA02KB TruST25 (ST) / KeyID 0x01?",
         'samples': ["02E3002FCD4038", "E71B844BCE76C8110B36E5B1E1C0410381BD994F226D5C11D84CA6697A5EB572",
                     "02E3002FCD4205", "5BE94577A06BC3030B1B3CECDC846E8128DDF81008DDEECBAF78CE91CDA27DBD",
                     "02E3002FCD44BE", "895EE509DE9D98E2FDAC7ADCC976F24B085D73D063986EF59FE260D9BE08D28C"],
         'pk': "041D92163650161A2548D33881C235D0FB2315C2C31A442F23C87ACF14497C0CBA"},

        # TruST25 (ST25TN) - KeyID 0x05 (b31-b24 of block 45)
        # curve=secp128r1, hash=sha256 - from block 52 in ST25TN, followed by ascii UID
        {'name': "ST25TN512/01K TruST25 (ST) / KeyID 0x05",
         'samples': ["020339A5940000", "A5E968CEDD7278C46F0FF7ECABAD649C229BCA444915D307E69C1945FA95C9C6",
                     "02643AFD04A000", "0938D86193C603E1B30B17C8117A930205CAC1A8CE88F0EA269FCE2A44244D7B"],
         'pk': "0440004F974F7C76BC8718E523D85FA7B354A9A992BFA966CB8219242F9D274FD6"},

        # TruST25 (ST25TV) - KeyID 0x04?
        # curve=secp128r1, hash=sha256 - from block 63 in ST25TV, starting with KeyID ?
        {'name': "ST25TV02KC TruST25 (ST) / KeyID 0x04?",
         'samples': ["E00208000A227AAA", "FE458A550ACD30B45B9C4D10676FC63E30AC69027217547FBDF04A8F7942AD3B",
                     "E00208000A17DB6E", "B3ED5025F16455AAC6012CA1C1FC5F94F8C805AF1EF6A86B646081C9916CDD2E",
                     "E00208000A174F60", "AB42F1FA68D7470F91D4ED77C4D951B1B4AAB006812BE194BFEB7AAE48E4FA45"],
         'pk': "04101E188A8B4CDDBC62D5BC3E0E6850F0C2730E744B79765A0E079907FBDB01BC"},
    ]
    succeeded = True
    for t in tests:

        print("Testing %-40s" % (t['name']+":"), end="")

        curvenames = guess_curvename(t['samples'][1])
        recovered = set()

        for c in curvenames:
            for h in [None, "md5", "sha1", "sha256", "sha512"]:
                recovered |= set([(c, h, pk) for pk in
                                  recover_multiple(t['samples'][::2], t['samples'][1::2], c, alghash=h)])

        if (len(recovered) == 1):
            c, h, pk = recovered.pop()
            pk = binascii.hexlify(pk).decode('utf8')
            if pk.lower() == t['pk'].lower():
                print("%15s/%-8s ( %s )" % (c, h, color('ok', fg='green')))
            else:
                succeeded = False
                print("%15s/%-8s ( %s ) got %s" % (c, h, color('fail', fg='red'), pk.lower()))
        elif len(t['samples'])//2 == 1:
            recovereds = [(c, h) for c, h, pk in list(recovered)
                          if t['pk'].lower() == binascii.hexlify(pk).decode('utf8').lower()]
            if len(recovereds) == 1:
                c, h = recovereds[0]
                print("%15s/%-8s ( %s ) partial" % (c, h, color('ok', fg='green')))
            else:
                succeeded = False
                print("                         ( %s ), got" % color('fail', fg='red'))
                for c, h, pk in list(recovered):
                    print(c, h, binascii.hexlify(pk).decode('utf8').lower())
        else:
            print("                         ( %s )" % color('fail', fg='red'))
            succeeded = False

    print("===============================================================================")
    fail = color('fail', fg='red')
    ok = color('ok', fg='green')
    print("Tests:                                                                   ( %s )" % [fail, ok][succeeded])
    print("")


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "selftests":
        selftests()
        exit(0)
    if len(sys.argv) < 3 or len(sys.argv) % 2 == 0:
        print("Usage:   \n%s UID SIGN [UID SIGN] [...]" % sys.argv[0])
        print("Example: \n%s 04ee45daa34084 ebb6102bff74b087d18a57a54bc375159a04ea9bc61080b7f4a85afe1587d73b"
              % sys.argv[0])
        exit(1)
    uids, sigs = sys.argv[1:][::2], sys.argv[1:][1::2]
    once = True
    curvenames = guess_curvename(sigs[0])
    for c in curvenames:
        for h in [None, "md5", "sha1", "sha256", "sha512"]:
            recovered = recover_multiple(uids, sigs, c, alghash=h)
            if recovered:
                if once:
                    print(color('curve=%s', fg='yellow') % c)
                    once = False
                print(color('hash=%s', fg='yellow') % h)
                print("Possible uncompressed Pk(s):")
                for pk in list(recovered):
                    print(binascii.hexlify(pk).decode('utf8'))
        once = True
