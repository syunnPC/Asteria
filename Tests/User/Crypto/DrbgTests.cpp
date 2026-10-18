// SPDX-License-Identifier: Apache-2.0
#include "User/Crypto/Vectors.hpp"
int main() noexcept
{
    using CryptoTest::Cleared;
    using CryptoTest::Empty;
    using Asteria::User::Crypto::Equal;
    using CryptoTest::Hex;
    using Asteria::User::Crypto::HmacDrbg;
    using CryptoTest::Output;
    using Asteria::User::Crypto::Result;
    using CryptoTest::Work;
    Asteria::TestSupport::TestContext context{"HmacDrbg"};
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"ca851911349384bffe89de1cbdc46e6831e44d34a4fb935ee285dd14b71a7488"};
        const Hex Nonce0{"659ba96c601dc69fc902940805ec0ca8"};
        const Hex PersonalizationString0{""};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex ReturnedBits0{"e528e9abf2dece54d47c7e75e5fe302149f817ea9fb4bee6f4199697d04d5b89d54fbb978a15b5c443c9ec"
                                "21036d2460b6f73ebad0dc2aba6e624abf07745bc107694bb7547bb0995f70de25d6b29e2d3011bb19d276"
                                "76c07162c8b5ccde0668961df86803482cb37ed6d5c0bb8d50cf1f50d476aa0458bdaba806f48be9dcb8"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"d3cc4d1acf3dde0c4bd2290d262337042dc632948223d3a2eaab87da44295fbd"};
        const Hex Nonce0{"0109b0e729f457328aa18569a9224921"};
        const Hex PersonalizationString0{""};
        const Hex AdditionalInput0{"3c311848183c9a212a26f27f8c6647e40375e466a0857cc39c4e47575d53f1f6"};
        const Hex AdditionalInput1{"fcb9abd19ccfbccef88c9c39bfb3dd7b1c12266c9808992e305bc3cff566e4e4"};
        const Hex ReturnedBits0{"9c7b758b212cd0fcecd5daa489821712e3cdea4467b560ef5ddc24ab47749a1f1ffdbbb118f4e62fcfca33"
                                "71b8fbfc5b0646b83e06bfbbab5fac30ea09ea2bc76f1ea568c9be0444b2cc90517b20ca825f2d0eccd88e"
                                "7175538b85d90ab390183ca6395535d34473af6b5a5b88f5a59ee7561573337ea819da0dcc3573a22974"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"5cacc68165a2e2ee20812f35ec73a79dbf30fd475476ac0c44fc6174cdac2b55"};
        const Hex Nonce0{"6f885496c1e63af620becd9e71ecb824"};
        const Hex PersonalizationString0{"e72dd8590d4ed5295515c35ed6199e9d211b8f069b3058caa6670b96ef1208d0"};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex ReturnedBits0{"f1012cf543f94533df27fedfbf58e5b79a3dc517a9c402bdbfc9a0c0f721f9d53faf4aafdc4b8f7a1b580f"
                                "caa52338d4bd95f58966a243cdcd3f446ed4bc546d9f607b190dd69954450d16cd0e2d6437067d8b44d19a"
                                "6af7a7cfa8794e5fbd728e8fb2f2e8db5dd4ff1aa275f35886098e80ff844886060da8b1e7137846b23b"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"5d3286bc53a258a53ba781e2c4dcd79a790e43bbe0e89fb3eed39086be34174b"};
        const Hex Nonce0{"c5422294b7318952ace7055ab7570abf"};
        const Hex PersonalizationString0{"2dba094d008e150d51c4135bb2f03dcde9cbf3468a12908a1b025c120c985b9d"};
        const Hex AdditionalInput0{"793a7ef8f6f0482beac542bb785c10f8b7b406a4de92667ab168ecc2cf7573c6"};
        const Hex AdditionalInput1{"2238cdb4e23d629fe0c2a83dd8d5144ce1a6229ef41dabe2a99ff722e510b530"};
        const Hex ReturnedBits0{"d04678198ae7e1aeb435b45291458ffde0891560748b43330eaf866b5a6385e74c6fa5a5a44bdb284d436e"
                                "98d244018d6acedcdfa2e9f499d8089e4db86ae89a6ab2d19cb705e2f048f97fb597f04106a1fa6a1416ad"
                                "3d859118e079a0c319eb95686f4cbcce3b5101c7a0b010ef029c4ef6d06cdfac97efb9773891688c37cf"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"369f0eec011db3db44971ab16371c7a8de327a4852bd34226e0f25358e296ce6"};
        const Hex Nonce0{"ca6043750aa99545d1597f71d583246f"};
        const Hex PersonalizationString0{""};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex ReturnedBits0{"b507091b56fc9e9cd90fe4c466b5a132615df2d4a18f73302f1d72a416b993c4c207388699e645f6048f59"
                                "5fbc7c356f85f683040a1ccc3155cfe4243f169f0f3e8b2ba5fb33b56a090e553342bd543134af325baa23"
                                "e4cdd114c429253c8ff9a0239d95ded339e412e23983454dd5091822b1e2712b298b319ab3d4ddef3b2c"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"8ee1df3d864bca351263fa00489d73d4a14c92f022a7cc2473695f4aa28ac496"};
        const Hex Nonce0{"7772269c9b6fa377349729a2a20bc1a6"};
        const Hex PersonalizationString0{""};
        const Hex AdditionalInput0{"e60fd2e75dd0693ab0cdd0bebcf39be34aa2eef17f186e40c97426e91ddc3fbc"};
        const Hex AdditionalInput1{"45daae78c6ff1b619460be949b6c6d02e1daddccd6839a38d5f631ac704886bd"};
        const Hex ReturnedBits0{"0d44fc2c64abf7e2deb1d4796e244afcd2cbcf543b52c5756b7395eef49480da5a0dc060d7d6b0b5beb665"
                                "5ed67d13e903e8731b482fff8bb8abd96323a5b4e6b342b1d665fcba0fdd146d41afe3c413fdfb90883170"
                                "afa62a1fe81d0dd2ad87b2b7db73ef15c5cbdaad876e3f279a0702f6f19a5f523615557aedf62e347d5a"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"a8c77ba575b8468bb5f99220de43d466cdb1d91ac3253c3be27cf18f82520624"};
        const Hex Nonce0{"052b3c6dd0ee77fb8b0980c38ffa2cdf"};
        const Hex PersonalizationString0{"4f074ac6ccb7c21c9589fa223428af0e860ae31fe008ecbf520653b9be235ea4"};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex ReturnedBits0{"5769beeaae2879ca0d7227d2dfc51bd3b7ac0c82c9ccaae84b2db60aecc7e06bb5989418865424cfe75f70"
                                "14b389a46870e222226e6c1a90a4f16208636f635076793b2ff8c99f93588ae0f93be1086a82f45c786784"
                                "f683d529157268984d7c13add196bfa0c7ddb314ce9375b2c13930209c52ddae347d064ed5d811f78065"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"191c4f0bb2853d9392dbab6defc8ca2eae6cb47d9a412d0490db7d08244cb70f"};
        const Hex Nonce0{"9044a8503b55ef8f40063fcf066994cb"};
        const Hex PersonalizationString0{"914fbe6c98771f1cec56223e4493505a31dcde5f9700a121be232c044f06a10f"};
        const Hex AdditionalInput0{"b57d926494b263f4ffec0190e73ac6699e1e40f162b5a6098d3a53a18dcb68b0"};
        const Hex AdditionalInput1{"275e7cbb53ced5fb5f063ce257d36e8319b64c89fd728f78701cc8168fa4aaa7"};
        const Hex ReturnedBits0{"2cf8b9a153666c8655aaaec79a233d4d715117d62d2ebc1902faba663717b9653a40ad4776bf4927067511"
                                "88c2461487e47a6567370b4946e455bf5caefb14e5d8fac20d9c54bde3235e9406705fee3eaab9a1ac4da9"
                                "497f98a457d1de4bbcefdbf8d72ecafe52e481d140a360fb1a3f5404f3a2f8a361a792bc0e5ff7430bbc"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"bf834dd99be9d58e3f6eac7664be2922c5fbcc99b8337037398e72757452b5f0"};
        const Hex Nonce0{"179123b7c887982acd3b4a37f3d87283"};
        const Hex PersonalizationString0{""};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex ReturnedBits0{"c46477fc8d4c9c2097826f24f66f65bce6888d9766ee2264661f0c32ab452a0f7b2d1705e2b22873a22fba"
                                "e0ad599494970b2780881b0a89dbf237a5c5b868114ba2004fb737ba22985368ab2e1940369888f484e2d6"
                                "28c7426adefd35712dd52b91abe48132974cf97a545140d8820da92a6a301e3f27e6b3160b1f4c1dba91"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"f3428ae375e688496eb2fb0cd772f429a2ca9a76c032d3c9463cb50e0b5890a3"};
        const Hex Nonce0{"4cb6959e2fca6e7d20d4fac254f8a7de"};
        const Hex PersonalizationString0{""};
        const Hex AdditionalInput0{"0f5e71a5804e9405e4f8676aba4aacfa4e727e0815a80b90f581fcfba078f842"};
        const Hex AdditionalInput1{"7f80c2e83cb9ece1648b04730a6cae9b8f7cea3fd5e2372324100f9642d26f4b"};
        const Hex ReturnedBits0{"7df486701b3a8e168094531aca081818d619697bf6c52e65a507a8a100cfd3a12d6fee64707840af7d08ef"
                                "76d5cc16485157887800bb1dd5193b99496dd5009c53050972ff006739f198d86f9815fd014e54d4dc0f99"
                                "b364aa77b74ad98c33dc3d007229de7a9a131ea388fc838ac966fc52f4924382ec288fc80d20b046df3b"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"e50153d9d7114aa4b8535482f8c4c8f187e4bc12a59d35c51f3fae9970d1f778"};
        const Hex Nonce0{"1175f514a24b02cdbd1d90396f92c51b"};
        const Hex PersonalizationString0{"7ba1b78349569a81902cbf95b5127cfdcc5bfaa761c349f1930c62cf536abd0d"};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex ReturnedBits0{"924462b8099a1019cb8837342194ad10f24b3c70b0e3b7ed8f28063ee5ad34a725a3311557ec63c3044416"
                                "e785739ed4ff386d6f5e5bf53d23c45f62acf725fa548b4c564e562a458424a63dab7771e4adb68fcdde2c"
                                "250413bb7e95c47f117d7bfa85ea3cf7034fe52b2611fa5deb3f7be98e27b8e5adb83f97e897d4e0cada"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"ea4da988e6c5f0685a587bcba36eabab586a87e2d724708078bb3766a80546ec"};
        const Hex Nonce0{"9221ed63f8003b6e95f7fc8043f5b01c"};
        const Hex PersonalizationString0{"93dc2c6b8499308ddf040ccf81464f482f09ea44be246f8c09181d488c28606d"};
        const Hex AdditionalInput0{"576cd452c79649d871d061bd8e1d7ffbac4687a72e25158c9e1dd87f7194f571"};
        const Hex AdditionalInput1{"c30f97e178b95ea29c60f9220bb23b15e9f0dcfacaedf1e825907327d732b0b6"};
        const Hex ReturnedBits0{"13ac0a60c6ff5a08f580bd1c039172fced4079a0dbe984242e7b0e113ef6694cece20e53162ec533d1fbef"
                                "12eed5a10a8b12829188baa15dc52883c9a8317e9c084d3e075221035241ec050b0bd498e5e27fd97df86c"
                                "22fbc5776d2edc273ca930c0a95877f4201378bcc3173556e9be46b0d55b340de69612c844166c10327f"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"0398eea9b1887385fdd440f46c475829e5a15d73e0fdc22e1d9290852a0fa0d9"};
        const Hex Nonce0{"9c7b1c1fa7491b8c7421854427ddc1c3"};
        const Hex PersonalizationString0{""};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex ReturnedBits0{"d2bdd8ffe6d840380f7dacc3ce913f10567b7ad40d3e319880c9ace504043d5158cb0679aa374ebc4d4693"
                                "620ce9ea372ed9ac7b95d90a098467cd4b6f491c41d86ae365a4c0da37690b1748fbe76f5312da9399565c"
                                "b691710c15e1e3a83b4e61f7e68c6727b9a2a5bcf752d11f80736a6f1ce1553e92d3a388ac83568651c1"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"5643776352534d25cffa8f62c071dda1cf6635008aca89aaaa574b564536e3bf"};
        const Hex Nonce0{"f79149ab6cf92a106931e51f0541c689"};
        const Hex PersonalizationString0{""};
        const Hex AdditionalInput0{"125edfcaa0cfa26f2d3ade21f3c7b273e0c4a9211b5b6c9ffafcef4ed1cc9112"};
        const Hex AdditionalInput1{"c1fc75f96f393df228685e1ae71539b7d7db0de05887bbdf6236e465b0014458"};
        const Hex ReturnedBits0{"52e2a447da5e55d0116aa47e1dafa35aeacdf777c70c3bd6291af7e5a600c5190541669e344f5d2094e014"
                                "149132743bd8d18c8938399cbb3ac9b99978b4e92f96d3c3a9b4c6ec90bd9eb45d1ccb7935488e490feeaf"
                                "23b44c6d9d8d99f953dfb67e3897cd03050cf4d85c8c3f124a4c8b90e1bba98f7b9f12898917fccbf6cf"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"30197171d85c81d6526474b43cb5efafeba746119ff10ef2a96d9b30f40a354b"};
        const Hex Nonce0{"c634e60d27088ed0cab663af404706bf"};
        const Hex PersonalizationString0{"1f0977ef52e535260e4f5a0d749fc6b2e9752e0f10848ba939a8b1ec3320b333"};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex ReturnedBits0{"af1a3ad8c297300fd1a7e32a2d306f8265d287aa9fa1e3727b60200ca8786bb2e4e4a44cdd9e12ecf396fb"
                                "c33d067c8167e202c5a524d0701b30be08697ec431dc497473046a61b69e3becb128c23d857c5f17442760"
                                "eef220102d38be11e21fb4cbe5398267fae7de5aa3e29f79c30c70ee39a7c06e05874e7a2b8c1735d6ee"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"f074a8cf417a9a4c4aade25f530567fd7a1410a074f3b0edd664bbc430ddb250"};
        const Hex Nonce0{"d3c0823b6d28a42d5f0fc01496d32859"};
        const Hex PersonalizationString0{"972527fe90601de9d13a050c7e49d556d0de6b0e75e0619807ade2178eefe47d"};
        const Hex AdditionalInput0{"0dc678372c9f24230d15acd1d36b13294c58b76f2847397fbc32dfada12b8e51"};
        const Hex AdditionalInput1{"59874caea33944638e1e11fa3626fa2bc26d4502120c17e0e198d04f9ef0ff95"};
        const Hex ReturnedBits0{"79db15ff50059ce58dcd44553f5cb6a19554cf35d2b64c869336a797cef93b24c64b716aaa11cd82dca014"
                                "3279ed7cb2698d7cd726241ca17b5ce6831b08ae84dd57f95b11c07f7fef1d381eb0b7fd535b902ccede73"
                                "538155f30100fd13ff007806b367f5032561338a92541f441725eab17996dd58e9870025d98b4752b547"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"06032cd5eed33f39265f49ecb142c511da9aff2af71203bffaf34a9ca5bd9c0d"};
        const Hex Nonce0{"0e66f71edc43e42a45ad3c6fc6cdc4df"};
        const Hex PersonalizationString0{""};
        const Hex EntropyInputReseed0{"01920a4e669ed3a85ae8a33b35a74ad7fb2a6bb4cf395ce00334a9c9a5a5d552"};
        const Hex AdditionalInputReseed0{""};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex ReturnedBits0{"76fc79fe9b50beccc991a11b5635783a83536add03c157fb30645e611c2898bb2b1bc215000209208cd506"
                                "cb28da2a51bdb03826aaf2bd2335d576d519160842e7158ad0949d1a9ec3e66ea1b1a064b005de914eac2e"
                                "9d4f2d72a8616a80225422918250ff66a41bd2f864a6a38cc5b6499dc43f7f2bd09e1e0f8f5885935124"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP reseed",
                       rng.Reseed(Work, EntropyInputReseed0.View(), AdditionalInputReseed0.View()) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"05ac9fc4c62a02e3f90840da5616218c6de5743d66b8e0fbf833759c5928b53d"};
        const Hex Nonce0{"2b89a17904922ed8f017a63044848545"};
        const Hex PersonalizationString0{""};
        const Hex EntropyInputReseed0{"2791126b8b52ee1fd9392a0a13e0083bed4186dc649b739607ac70ec8dcecf9b"};
        const Hex AdditionalInputReseed0{"43bac13bae715092cf7eb280a2e10a962faf7233c41412f69bc74a35a584e54c"};
        const Hex AdditionalInput0{"3f2fed4b68d506ecefa21f3f5bb907beb0f17dbc30f6ffbba5e5861408c53a1e"};
        const Hex AdditionalInput1{"529030df50f410985fde068df82b935ec23d839cb4b269414c0ede6cffea5b68"};
        const Hex ReturnedBits0{"02ddff5173da2fcffa10215b030d660d61179e61ecc22609b1151a75f1cbcbb4363c3a89299b4b63aca5e5"
                                "81e73c860491010aa35de3337cc6c09ebec8c91a6287586f3a74d9694b462d2720ea2e11bbd02af33adefb"
                                "4a16e6b370fa0effd57d607547bdcfbb7831f54de7073ad2a7da987a0016a82fa958779a168674b56524"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP reseed",
                       rng.Reseed(Work, EntropyInputReseed0.View(), AdditionalInputReseed0.View()) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"fa0ee1fe39c7c390aa94159d0de97564342b591777f3e5f6a4ba2aea342ec840"};
        const Hex Nonce0{"dd0820655cb2ffdb0da9e9310a67c9e5"};
        const Hex PersonalizationString0{"f2e58fe60a3afc59dad37595415ffd318ccf69d67780f6fa0797dc9aa43e144c"};
        const Hex EntropyInputReseed0{"e0629b6d7975ddfa96a399648740e60f1f9557dc58b3d7415f9ba9d4dbb501f6"};
        const Hex AdditionalInputReseed0{""};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex ReturnedBits0{"f92d4cf99a535b20222a52a68db04c5af6f5ffc7b66a473a37a256bd8d298f9b4aa4af7e8d181e02367903"
                                "f93bdb744c6c2f3f3472626b40ce9bd6a70e7b8f93992a16a76fab6b5f162568e08ee6c3e804aefd952ddd"
                                "3acb791c50f2ad69e9a04028a06a9c01d3a62aca2aaf6efe69ed97a016213a2dd642b4886764072d9cbe"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP reseed",
                       rng.Reseed(Work, EntropyInputReseed0.View(), AdditionalInputReseed0.View()) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"cdb0d9117cc6dbc9ef9dcb06a97579841d72dc18b2d46a1cb61e314012bdf416"};
        const Hex Nonce0{"d0c0d01d156016d0eb6b7e9c7c3c8da8"};
        const Hex PersonalizationString0{"6f0fb9eab3f9ea7ab0a719bfa879bf0aaed683307fda0c6d73ce018b6e34faaa"};
        const Hex EntropyInputReseed0{"8ec6f7d5a8e2e88f43986f70b86e050d07c84b931bcf18e601c5a3eee3064c82"};
        const Hex AdditionalInputReseed0{"1ab4ca9014fa98a55938316de8ba5a68c629b0741bdd058c4d70c91cda5099b3"};
        const Hex AdditionalInput0{"16e2d0721b58d839a122852abd3bf2c942a31c84d82fca74211871880d7162ff"};
        const Hex AdditionalInput1{"53686f042a7b087d5d2eca0d2a96de131f275ed7151189f7ca52deaa78b79fb2"};
        const Hex ReturnedBits0{"dda04a2ca7b8147af1548f5d086591ca4fd951a345ce52b3cd49d47e84aa31a183e31fbc42a1ff1d95afec"
                                "7143c8008c97bc2a9c091df0a763848391f68cb4a366ad89857ac725a53b303ddea767be8dc5f605b1b95f"
                                "6d24c9f06be65a973a089320b3cc42569dcfd4b92b62a993785b0301b3fc452445656fce22664827b88f"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP reseed",
                       rng.Reseed(Work, EntropyInputReseed0.View(), AdditionalInputReseed0.View()) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"ff0cdd555c60464760b289b7bc1f811a41fff72de59083858c020a1053bdc74a"};
        const Hex Nonce0{"7bc099285ad5621993b639c4a94c376b"};
        const Hex PersonalizationString0{""};
        const Hex EntropyInputReseed0{"14fc6c9b178db644a8cd7130a4cf051678c8f4fa8f24c27b0a531338a5ce8589"};
        const Hex AdditionalInputReseed0{""};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex ReturnedBits0{"2f2620347bddcaa2943685346bbf31c44081f8665f3ddb2b42ae1416a74c4b77fab3fa19aeecc547e76c8c"
                                "be6ad1f100a3fc8b2ce2a1ea3a3dd7cfad46c1b27830b940ba18d09e9b7fa902bb760669b1735cc7b7bd39"
                                "052da7f2626fa87000cffada410019d053386ad808bd3c0cfcf56b91879eb8d3f932ee2d185e54f31b74"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP reseed",
                       rng.Reseed(Work, EntropyInputReseed0.View(), AdditionalInputReseed0.View()) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"d5303207d58bffb97e0772dc848e7e32dfe2f517fcc9b82f256dcbbbe225a543"};
        const Hex Nonce0{"478f5d6ee7101835a177bd002ac75955"};
        const Hex PersonalizationString0{""};
        const Hex EntropyInputReseed0{"e2b00122b868747633010cf2e505db7fe89b197f0847508ec385d2180c97b962"};
        const Hex AdditionalInputReseed0{"1c2a88e25d1711c7862a849eb9a217c2a4219031a0d2e0c2c2dfb5f160b2528b"};
        const Hex AdditionalInput0{"282e5c2989d4df5e1ce476bf05057b7560cab5447b15992951db78f7a92767d9"};
        const Hex AdditionalInput1{"3a5b9e896338713c7707aa03360a3027f76e2418bdced7d3e8062196e2721887"};
        const Hex ReturnedBits0{"623af8a786c2303f1248eda345d3a80df15a6be6cd34973d68c454ea1399390a41835266c27d0d2efc7bab"
                                "2207022b2adbd8de654937b49bbf620a716fb0c69911c39b2f96ace53d81fd1bc015364dfdb4b226f216a2"
                                "a129fd0d1a061d74f4aaf6cc8871e015a480e527aad612f40178ad40d4f790b6f81de9b4669b194b799f"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP reseed",
                       rng.Reseed(Work, EntropyInputReseed0.View(), AdditionalInputReseed0.View()) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"28ba1a661632efc8ecced5f51b791300fb3b55b05d041708638de4beb757a9e5"};
        const Hex Nonce0{"76828796aff07f55795cb54713c77ed4"};
        const Hex PersonalizationString0{"40933fdcce4159b0955111f844471b0db85b73bdd2b78c468dd39e2a9b29aef2"};
        const Hex EntropyInputReseed0{"a5f542b04aaa5dbc931e47019feb38962616c57af09b7c1df83f2b860ff76586"};
        const Hex AdditionalInputReseed0{""};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex ReturnedBits0{"65e5aa47b385f1ea42b231b9fe744253b8598859d7011e525f5a2a1ad32a972a850802c60a2be19be27006"
                                "3a3cfbeaae954f10b122352de6a08ac410e0991653aab271b360fe9191cf5addcccced8c4acfb614570499"
                                "92988fd7a9acca1f1bca35f1475813694a39988e5fac9f4ac0572286bc462582ad0af78ab3b85ec17a25"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP reseed",
                       rng.Reseed(Work, EntropyInputReseed0.View(), AdditionalInputReseed0.View()) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"40f4a4344082039f5452c3fd21aba9393d253c558728346b6954b00f0c64beb1"};
        const Hex Nonce0{"f8e297eedaf4384a12d91e5b3432c69f"};
        const Hex PersonalizationString0{"11716a84a8b4e5717b61f0827cc9e56e21c0acb9b40372f68902576b50c46ba5"};
        const Hex EntropyInputReseed0{"07402212d59681934d22d9bde4cb04d69d2fca68e184c526de933669bfa18f78"};
        const Hex AdditionalInputReseed0{"604e8c8e5fd3c9368288451929c9de0e7f537493d7adba56dc400961abebe501"};
        const Hex AdditionalInput0{"34cecc853e6e379bfd1e13b8eaed00db34602e33760c590e3f75f10ea2e26951"};
        const Hex AdditionalInput1{"82b7f37cb3ca1543927659fddb13d09507fe77c941c5d60d451a9103f1b0b5e4"};
        const Hex ReturnedBits0{"3ca3ebc8cdd8280c07213c5a5f1e937ba1294c9aa682ca1a3dee63842132b8da641b340861abe7512074f6"
                                "d1b9745a541ccf19cc1224967a9faa285e21d7572b3e669d9a3647c30d43c27974fbdb461d468149fc1b34"
                                "0d4ee6c640cfe194669d85d4a595bd1b78808e0496d0e2e38a1b9bc1d0e5273fa03cb5f041bfea7f1c9e"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP reseed",
                       rng.Reseed(Work, EntropyInputReseed0.View(), AdditionalInputReseed0.View()) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"6ae80303292391335bf9c9387fbd3bf615756c9c27c3478c87e260cf97d47110"};
        const Hex Nonce0{"01e16247dd4cae6499337d82784ea57f"};
        const Hex PersonalizationString0{""};
        const Hex EntropyInputReseed0{"035702ef4e112b173112c5851d07b279309863740d38d0d0720223e24017bbc0"};
        const Hex AdditionalInputReseed0{""};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex ReturnedBits0{"cf4315598fcd6af1315518c4bfbac0540c589635273548a7b507e7d2e685e5947b87ae257e58faf214f2b5"
                                "8ed10c3bd35f75f6c35dd6d441c93bcd42e71720102631b1a6a4ba247c175ed800cfca6e1e839b5aa90760"
                                "4ccfe6f984f6822e001ab02dd6634964f789cb107a977346693f3244c895e840dfa0edf7f14dc61d794f"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP reseed",
                       rng.Reseed(Work, EntropyInputReseed0.View(), AdditionalInputReseed0.View()) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"010935ec2370e1f0fedf365cd4e37666727cc33eb799f38b1cfd226b7037d9b1"};
        const Hex Nonce0{"e09e498a94860ac567ae911bb218f36e"};
        const Hex PersonalizationString0{""};
        const Hex EntropyInputReseed0{"d21a3b9a439bb26fa4b4009807c5cafed3ccff19b2b0608a29f0ef6289f27abb"};
        const Hex AdditionalInputReseed0{"204fa3482fe99b6670ec70b7f2558a5ce9caa98113bad174b85a719410010867"};
        const Hex AdditionalInput0{"4f61b22033e1cc256f5c677e91882914abca6a5a3f716af0ab2c652ff8bce5f8"};
        const Hex AdditionalInput1{"ce1eb61f9af9727844666e283f2ae69eb7c91d098535fbf2ac7b0584ba81560c"};
        const Hex ReturnedBits0{"503e08620268aff772e06603679a7509b4bd590787375a4319fd1f7c7ca7261aa1ef336d862096b4cb98ba"
                                "97c5e96905e410e719fe2a2de1be621c5a537d1596c7e20db9b242523fc926e22e2826bddabdca1c0b8e2f"
                                "db32c87044eb6e7760c6634bd9b96d385f98fb0f8a273dc21bb7dbecc29ff69bd691688db2a4b013576c"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP reseed",
                       rng.Reseed(Work, EntropyInputReseed0.View(), AdditionalInputReseed0.View()) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"633d32e3005f78114723b3ea5ac121ba74aa00c52d939667e30c3351b38549f7"};
        const Hex Nonce0{"37afff504a2d8ac168c68e24d0fe66f6"};
        const Hex PersonalizationString0{"9f1699c99d60b085bc61cb110ef8ab590d82a970021c3c6a5d48021c45de4956"};
        const Hex EntropyInputReseed0{"3e3347c547f17f4d0b9f46405a54eedd7e980d06a215ec15e89316ab743b7547"};
        const Hex AdditionalInputReseed0{""};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex ReturnedBits0{"6e38e82962d707ce9a6ac383a738a748f975eb785611fad5e3f5a4fe44d7b59a98137a2bcdc35f9ee9a1e2"
                                "1bb17df1665cd1397625a177247e2e329a660140636141560610a368bfd499c2e25be318aa4da9e7a352d1"
                                "15db8282ed8d79ecf9cd820360d3d2d1a58a93e040f5554887ce6c9858bc2bb102249980a858498abcda"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP reseed",
                       rng.Reseed(Work, EntropyInputReseed0.View(), AdditionalInputReseed0.View()) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"d712e0448c7f07ffc32cb24d4a13980f63a95c676d332f3d96dff8faa867286b"};
        const Hex Nonce0{"eae6e959651245cc5420455b264bddc9"};
        const Hex PersonalizationString0{"82e3cc515480a6d3fefc6687ac488ce04c0cb0361fb87098104355bfef4b41fe"};
        const Hex EntropyInputReseed0{"b34a0fdbd1082df4fb3404502cb99a2b564b7a7335e485a907693b8be256dd10"};
        const Hex AdditionalInputReseed0{"e500f6f8af3c4ff61f46f933f80e80e4d08376349511de5a2d3394855e5edf33"};
        const Hex AdditionalInput0{"87c571b98a05b372cbb53429a37d91295f6f8892813ba23c3c21327f83f9f55d"};
        const Hex AdditionalInput1{"72d1895e1c4c3093452447cdb3519c8dbdcaf90fa0821ab5ced0079c03ab8dd0"};
        const Hex ReturnedBits0{"ce50ca00cb699b6dfdf0b997b8a425d5d665150579c01232d51b6b296138fd39da0349c9fe1e1c47cb2589"
                                "342cc9a9dd9d17671a68a4915be152826c3245d51d87b37275a5cf4be9b4c90bfe752065eb93bb99679b25"
                                "ace57ac6bbd4c7a30ee489cff74f2d11a38782f05301db7c60c9659f08f237de354ff7a8ee10b8d93460"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP reseed",
                       rng.Reseed(Work, EntropyInputReseed0.View(), AdditionalInputReseed0.View()) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"ea28926cd5df4fefd572c9103d87ffb04f599da95e1e6fecb84f53f73fd00d6c"};
        const Hex Nonce0{"cb40e16655b9a2c71e8e3677b9ea6c6f"};
        const Hex PersonalizationString0{""};
        const Hex EntropyInputReseed0{"3d2d1db88b8462787a5576c95fd660734fb681f894e8efc47e3be3bfc3098e40"};
        const Hex AdditionalInputReseed0{""};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex ReturnedBits0{"e929c6e749c5175031dcc926bce8d529147b5e940f61d0ba1f02831c80c27a23cd4b5ffb507c7d09a77e4c"
                                "8427e29010cf1c8021a80ca29504caa350a27d6ca4554fe4d8b0235554f251a59ec6729d802b473083b0bd"
                                "6ca83f6d945b3d1de2b706bdcc3b50ddef57847fff88a4498586ca6afe65e76c2d97f87ddea66f5563e3"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP reseed",
                       rng.Reseed(Work, EntropyInputReseed0.View(), AdditionalInputReseed0.View()) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"82d87758f16581b2bd8bd4374aba49a59b65cb953f753594240e69575d51b170"};
        const Hex Nonce0{"c12ea8a42dbef64b9b3abbae9222f94a"};
        const Hex PersonalizationString0{""};
        const Hex EntropyInputReseed0{"f9bcb64209e8f68e6e3f996196ab445dba64568c7e572dcd2378e916449e39f7"};
        const Hex AdditionalInputReseed0{"987d93237e7e52d1ef3941407e8739f7d95991bad92a49ad6a681dcab1644049"};
        const Hex AdditionalInput0{"8dd4dc1e62114a0c7e52537607ea8313fa4128f486d871a8bb8adbcd9bf46c42"};
        const Hex AdditionalInput1{"e16721a200d3f18abfd154eb0dc2c9b7c245e8a2a644d59c2e888d4de11652de"};
        const Hex ReturnedBits0{"ba00a49886a6c34ebcd649bb93989cabb9c1d11f9c53f721c99ab125a6cf4726a79713c2683ddae6ae7939"
                                "be465e9a2b95d008af76db42973a6b63a33bc662d99afd9bd4c7aaadc116da5d11db66f2fc27bfd471ff51"
                                "30b40f81c0da8de5ba09661165371818ea61f936b4fbd511efc2dc5a7d24dd56326a0e10b03f1f94465d"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP reseed",
                       rng.Reseed(Work, EntropyInputReseed0.View(), AdditionalInputReseed0.View()) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"22d8c62bcddf5da1dbdb093d6b1f663dde337343c56ccfd40acbe3302309eae0"};
        const Hex Nonce0{"2a4b8e66deaac38b70d9ffc20c585da5"};
        const Hex PersonalizationString0{"0a337038f4b4573ff43a4321a586ca777c30301267d82fdf937198ac56c7062c"};
        const Hex EntropyInputReseed0{"0b6fd179e44f149f062da4f66f829c3c58c4a0a4f75ac2a9e0240d43bec30e44"};
        const Hex AdditionalInputReseed0{""};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex ReturnedBits0{"046e19b4d8ab38dd08defdd0d7c30c8c75d5689ca26f2bc994ac7fca4fdbee80677dfbdd851e7722835844"
                                "dca79dec4a3fa82feb884df7d474bd972e12619bd5d6cb1b951eac47eec2581195b531534ede507af6f741"
                                "7fca84532be7ef5da6738dbf7aadfcd7cb888862b52ec773cf3fd00e6d4efb3071af9b70a49935a3ba38"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP reseed",
                       rng.Reseed(Work, EntropyInputReseed0.View(), AdditionalInputReseed0.View()) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"00efb9c7f02719ff5c7030ffa897a308d36c11ce27526340728bcd487c80457b"};
        const Hex Nonce0{"09cebd489d363b5578ddf30534ee6a7f"};
        const Hex PersonalizationString0{"27e38c624a8f934e931e195a0cbcf38e4e8d50108dc318743fb4b61cf78a7d14"};
        const Hex EntropyInputReseed0{"4c87234a9bb529aebb7278daa089753bd2b501d30677edb6cc31e38788fe0e21"};
        const Hex AdditionalInputReseed0{"0e4dddbe0034180b59303d527a938a447bad9e4a91787d1072e6f41350ff11e5"};
        const Hex AdditionalInput0{"cb25fccf929812b9fc66aea93e0cafb064e25b8c2989ae5078648ef529ecb487"};
        const Hex AdditionalInput1{"c1685a422e4a0673cea9948937a8fdaa77777066f501aa17493682a83d931e6a"};
        const Hex ReturnedBits0{"7569ff1ad01a56ab283c1f2357bd519e15c0be84b80cfe8ec6e26cf903aa8a17f52311a2458e48468122ce"
                                "1f4abff12920f7dffa86c46f06d744d198004bdd0b29b1b0f17712863df82406e2c2a2fb73ea99dc3969c7"
                                "e52aeaea031e0112fbf8d785426ae7c106d876a900ba54c4e9a1f3656990571c6d1fb56131cd1cdb1e68"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP reseed",
                       rng.Reseed(Work, EntropyInputReseed0.View(), AdditionalInputReseed0.View()) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput0.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, AdditionalInput1.View(), {Output, ReturnedBits0.Length}) == Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"9969e54b4703ff31785b879a7e5c0eae0d3e309559e9fe96b0676d49d591ea4d"};
        const Hex Nonce0{"07d20d46d064757d3023cac2376127ab"};
        const Hex PersonalizationString0{""};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex EntropyInputPR0{"c60f2999100f738c10f74792676a3fc4a262d13721798046e29a295181569f54"};
        const Hex EntropyInputPR1{"c11d4524c9071bd3096015fcf7bc24a607f22fa065c937658a2a77a8699089f4"};
        const Hex ReturnedBits0{"abc015856094803a938dffd20da94843870ef935b82cfec17706b8f551b8385044235dd44b599f94b39be7"
                                "8dd476e0cf11309c995a7334e0a78b37bc9586235086fa3b637ba91cf8fb65efa22a589c137531aa7b2d4e"
                                "2607aac27292b01c698e6e01ae679eb87c01a89c7422d4372d6d754ababb4bf896fcb1cd09d692d0283f"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR0.View(), AdditionalInput0.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR1.View(), AdditionalInput1.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"2cad88b2b6a06e703de46185ccb2ddcf5e0ee030995ebdf95cc4fbc38441f17f"};
        const Hex Nonce0{"32310770e04172c0cf91f6590cce44a4"};
        const Hex PersonalizationString0{""};
        const Hex AdditionalInput0{"ef6da5e6530e0d621749ab192e06327e995c3ac0c3963ab8c8cd2df2839ab5df"};
        const Hex AdditionalInput1{"44278b31ed853f0a510bd14650ac4b4971d8b426799a43511d016be68dedbb8d"};
        const Hex EntropyInputPR0{"448bfbc5ce9e3b9da3e9642daecd994dfe373e75253e8eb585141224eca7ad7b"};
        const Hex EntropyInputPR1{"afb57f69799c0b892b3015990e133698d543aa87829ace868e4a5e9525d62357"};
        const Hex ReturnedBits0{"4c7dfbe509dc5a3ac26998723c6a44cad20b197fc86117c778d1568ab828923862885e97198f77a1cb4511"
                                "3f5d78726a0f120aec94afc45f57c8dcc1cb092b343480012858ef5bc559f57023442209326ec4a54d91ca"
                                "3a77dfdf9e75f117cef50e6fd2dc9af6ddce8e6515b4a97357a97b6cd274f68a042fa41bbd7b7261b034"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR0.View(), AdditionalInput0.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR1.View(), AdditionalInput1.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"f7b90c797a4a376cdd9f5c435f5985e77f36ec1df1145a12072cbb2a0da378fc"};
        const Hex Nonce0{"d95202986d45896e9f4a65f2f353fa35"};
        const Hex PersonalizationString0{"61535c5c045e784267fd0d85f2861778fa53c8e8586af67cf5c9f21a28ebb656"};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex EntropyInputPR0{"130ab64f41a5d49d6a241e0260b4bb8a46a16c6ac9e234c84b5b26cdb518d459"};
        const Hex EntropyInputPR1{"f7670e817ac061ac60439be60982492000dc5da8bc6636bdac8b1cab03198dfd"};
        const Hex ReturnedBits0{"8df4e349f9ea43cc509ecb2b1124358cda2de1f5cc9315edca63610a413478d68b8bb49c2814c82ce571f6"
                                "e0a6780fa21c4b570610ee0c04d3edb92124f580f962d741330200c19885ca716502223247b728d66fbbeb"
                                "7c6cc25cfe9866b1450b346227c7663074c8b15d189f1c6edba172a53c733d67c1c69bd7aca7e62013cd"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR0.View(), AdditionalInput0.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR1.View(), AdditionalInput1.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"4294671d493dc085b5184607d7de2ff2b6aceb734a1b026f6cfee7c5a90f03da"};
        const Hex Nonce0{"d071544e599235d5eb38b64b551d2a6e"};
        const Hex PersonalizationString0{"63bc769ae1d95a98bde870e4db7776297041d37c8a5c688d4e024b78d83f4d78"};
        const Hex AdditionalInput0{"28848becd3f47696f124f4b14853a456156f69be583a7d4682cff8d44b39e1d3"};
        const Hex AdditionalInput1{"8bfce0b7132661c3cd78175d83926f643e36f7608eec2c5dac3ddcbacc8c2182"};
        const Hex EntropyInputPR0{"db9b4790b62336fbb9a684b82947065393eeef8f57bd2477141ad17e776dac34"};
        const Hex EntropyInputPR1{"4a9abe80f6f522f29878bedf8245b27940a76471006fb4a4110beb4decb6c341"};
        const Hex ReturnedBits0{"e580dc969194b2b18a97478aef9d1a72390aff14562747bf080d741527a6655ce7fc135325b457483a9f9c"
                                "70f91165a811cf4524b50d51199a0df3bd60d12abac27d0bf6618e6b114e05420352e23f3603dfe8a225dc"
                                "19b3d1fff1dc245dc6b1df24c741744bec3f9437dbbf222df84881a457a589e7815ef132f686b760f012"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR0.View(), AdditionalInput0.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR1.View(), AdditionalInput1.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"9e2371af63ce0a2d60f39cee6c78980184b8de108be128b91cefd4cc11445046"};
        const Hex Nonce0{"d19085ed7d6f9792a457727d0188c859"};
        const Hex PersonalizationString0{""};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex EntropyInputPR0{"d156908d7d175943c88c547ee0ed33603fbe9d512026c40abb35d736b72dd676"};
        const Hex EntropyInputPR1{"fa1f8951bbb16009e45178b3172f6fb7fbc2d146866bc2119d48a94e42f0e354"};
        const Hex ReturnedBits0{"c3f045d97ca01825c9735815f3788e2b0ce05cb5f8ea0b31cbdb8529d18ffe2c63acb9e8c92373ba752d15"
                                "f6a832539bdcf154d7b7981b59a7d3b992ce3885f70af21da7f1e5c1c7f2f381c57bd3cd5adf4d2ab135a2"
                                "f42a706988f9f5b83d955c0611695dae1b21d5ce44e2bf4b25bf7c49d42cdf07d48327caf9285f70627b"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR0.View(), AdditionalInput0.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR1.View(), AdditionalInput1.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"f751b4d6f05c9759e485afd370372d6ac2b70f50e38b69742a586769162dfefe"};
        const Hex Nonce0{"99d82a54ad070e2ac8b5ccd88905dfde"};
        const Hex PersonalizationString0{""};
        const Hex AdditionalInput0{"316cf45d19bce33d6de7c2a8f206f9383554126b526b51b2d7f88916109af19f"};
        const Hex AdditionalInput1{"bacdc91ed6847104e8547ca14bf994b70709a021480c9e506dbd53f402f34e98"};
        const Hex EntropyInputPR0{"336bdcef8bbe7dd599c7b404a6b700a65a4d018a2fbd9753e1dbe3b11ba85c96"};
        const Hex EntropyInputPR1{"e4b0cc5f564c7f39e319a96a30bdadc78e34f5270a4a65cca0653bb85edbf790"};
        const Hex ReturnedBits0{"604776b9ddee3aca601880341e9bd7d34123a38e73e00c14b89abc64472313ec2286c1b65d01821deb6ffa"
                                "5b99c5ef7aaf1499edcaed3d97a4d27b35df239e1a9e11b0b5813956f9f85691b73500ff309e65dac64cb4"
                                "5dc5d2b4cfe3b9253ae328ef47ee619c8f63017809b3713ad0dad10950b369aed030540e3bc6f0541705"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR0.View(), AdditionalInput0.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR1.View(), AdditionalInput1.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"03b4e89109763050e88f663af66c5ed1b16c6cb2a4ca4ba1985b1091b340f308"};
        const Hex Nonce0{"491c20862989be1b158021eb4220b6f5"};
        const Hex PersonalizationString0{"bf924bcd7149694942f2eae127d6791430d3977154d3faf42b901f380fffc7e4"};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex EntropyInputPR0{"469acc0d24b2cb422a64e485fdc768f1ed444d9ade5bfc7576f5bd505d7de67e"};
        const Hex EntropyInputPR1{"78991884940dedd29099a4056da68299ad6c9f90a10131c91e88cd9c36daa09c"};
        const Hex ReturnedBits0{"65370b7b4d425a8919ff1fa436fc93c16f12acdab5e1576ca452dfc6597ed06bd0eccf78c02711c3cbf736"
                                "3f6270c7ef376381771fca72eff43e37b2d70357b4406759346a8718bc276cbbd1f1811f8b96aa441c6b64"
                                "59d2a570e941cd3ff1ccd64470eabe6687404ac502515360d968e819025115e6b015233ecbec58512b88"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR0.View(), AdditionalInput0.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR1.View(), AdditionalInput1.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"cb05d9611b4abac7314f87090a145677448c0c8e89e0a84c495b96731c10da3f"};
        const Hex Nonce0{"7138b0f4b5a604cafc1d048a6a568a73"};
        const Hex PersonalizationString0{"97af18044ca160801059268a97d50950fa0c305cdbf315e29c9bbbc9ef194d88"};
        const Hex AdditionalInput0{"a7ee3fd3780d26e6304fd9341310f3c31f6f6101961de593a7233f877ead2e3b"};
        const Hex AdditionalInput1{"5d3790535f295b1361880cfd9825aa4564726ede97f4401354cdcd671595c31e"};
        const Hex EntropyInputPR0{"3a45af508904b042a9406efc72957c90a07ef44f9ff939747bf6444bc9d3b439"};
        const Hex EntropyInputPR1{"33e5f4b5162b5c80b6e7dfc7d920d3ac5602c81910259cb89f334fa8c541e51d"};
        const Hex ReturnedBits0{"a2231c9a7cf4b4947bced3e6831b696696579a4e315d72b798dbceec9cca1f119f41fbcc91e356d5556299"
                                "d825ba67c97069aeb65711d44de1ba300ea29468673ba867dcafa95909edd2291c4cc8ffd050bcdf69fe6c"
                                "58c1ca6ad11e9abca6644b259bdb0601d684aa324943c8ead80d349ca632eb8d150ae952f24c1f74e1f9"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR0.View(), AdditionalInput0.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR1.View(), AdditionalInput1.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"3cfd0ea75d8ef48ee4eaa4cf29c5e69d07bd7c8b518fb298308a9b038308760e"};
        const Hex Nonce0{"1e2bdfe38b9ed369fefa70b3d4d9c49f"};
        const Hex PersonalizationString0{""};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex EntropyInputPR0{"f63c556c14fc765478d59f579c627f863de944b2d0ff975f90822263d48d67e7"};
        const Hex EntropyInputPR1{"8fd379f6137c498f1c51fffc19de42c05fe79f9486b3acaeb652b73119ca5a43"};
        const Hex ReturnedBits0{"8e3443d66ca1bc912d07313af9533cf03cc338d812004e88d2ab047cdb0aff72ecaa4ecc574bbb8f836b5c"
                                "a5943aa21cca92502ae5cb3569ff2a86bf5b04914a278b6447d1a4a7c5c8f330157682d4141a220fbe9dc1"
                                "053a81954c0b5a637d2d4a6c0ea3ac92c9098f06ed3f33c5e156557f837d1db9984373a02c59aec64753"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR0.View(), AdditionalInput0.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR1.View(), AdditionalInput1.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"ee997dd55c63afeff72590640cce548a74ac06380255c57176aed445fa30d6bb"};
        const Hex Nonce0{"be03de59870e9f269c9b53c27ea149cb"};
        const Hex PersonalizationString0{""};
        const Hex AdditionalInput0{"f97f2759ae718f7495dab7490c217b94ff8d09d6cc219489b1d1ece38773a062"};
        const Hex AdditionalInput1{"6d72c6cef3765d167a59b5ca4803a57fd2a738a9b3df1ead1931784013de1c3b"};
        const Hex EntropyInputPR0{"95afa03a0bc6c7c7e0878bb274235bafb1555e47462328996019fb6177a2fa1c"};
        const Hex EntropyInputPR1{"563da79bd77ea9d8102e688c5293ff6fc6d3c8da51a6272a73dfef64759cac2d"};
        const Hex ReturnedBits0{"948731ba768520719367b798dea737be2f3a89baccd9338308ab4fbb656334c0776864dae21d8e05aee7ca"
                                "3420929eea65353b102b4e65d35af2c60003e2132d437ae18b08234a3a28e4d6e494ccc698413e3567dfbf"
                                "01b76382b3d320f649c0d9011859dfba0cf6a7326896aff04b395c490198fb91c80f82558a39c45360d4"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR0.View(), AdditionalInput0.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR1.View(), AdditionalInput1.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"ae3773814f89660421a0d5afad2446d19b7c593e331991b27a1fc8a3ef11326d"};
        const Hex Nonce0{"c024d24055fc0892aae7cdc6ad1f8102"};
        const Hex PersonalizationString0{"4a0026b9cf1f3e23017dd0eae89e43a7e95621ff662fe1d58c7df128d4ec7646"};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex EntropyInputPR0{"85c44bfb4c0bc8b708ad9b1572d35495388d412e07e05dd216bf2f4f4073ba2b"};
        const Hex EntropyInputPR1{"727e88f49178acb32ecb8c164c57fa4775e97083820edf1a00ba4cd3e5d3c84d"};
        const Hex ReturnedBits0{"0bfdb529f27f4625f59ee3d49af203aa6d98cca0bdfefa2b68c5cb6c1dbb6ff8199560dc0528202e519cd7"
                                "675afc6c5db7c2ee26030ebacf8ac91005284c86c2158a96a10a77073b265ba48cc135688a2737c2adc53b"
                                "f662a93a5e922dcc26e74f79167c161d17a07cce74116a3a4b3fd3299116287760ab5bb9eddd1d6d3eee"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR0.View(), AdditionalInput0.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR1.View(), AdditionalInput1.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"04e6323d8bd245b5c57831e1c56300b178b8d6ed2162a7ddc91963b87a8b4d81"};
        const Hex Nonce0{"9da21eba131174aa84d0f8e98605f29d"};
        const Hex PersonalizationString0{"70d0d74d7c5aa048b65b4e324f3de7d2ad2aa0b82897472fc9460feac7e796b8"};
        const Hex AdditionalInput0{"e44d5262671c8331c2cda2e5154820263b6df4a7e2eb0b3ff65735d4aa332cb9"};
        const Hex AdditionalInput1{"6e3bbd3a944c9a4db3a125b7a83a444c822ee2d7956f51ee680493650dc9396f"};
        const Hex EntropyInputPR0{"979944d614922aa2f8a865a7a1a64702923fbff85b798b3e1651429cb359e14f"};
        const Hex EntropyInputPR1{"8bcee4d660bba9e359cc3844859a77c7b81806de06ef1558fbaee8c35acb1cff"};
        const Hex ReturnedBits0{"e923a924fa6488219c80ae50a14aa261c832d2c3634e90c225921a32be8c61435ce53f178cb0e0576150b8"
                                "9d782685d6f0236211d8c09a22de30053db25c4b0fe9710ffb60e95e319f1f340f8522b65d4b10704b53eb"
                                "97aafbade8ae5bedfb1698ea3a0b23df152046420573c7bb4368bad450e49b5e30625b52a33d5975a509"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR0.View(), AdditionalInput0.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR1.View(), AdditionalInput1.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"0f02d1900573837284ce13529cfb02435a827ec8c83495520909f8d22dd39c62"};
        const Hex Nonce0{"77e0eed9762ff72f43b998cffdfb68c1"};
        const Hex PersonalizationString0{""};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex EntropyInputPR0{"b2d8be6f1a46e9b46ed0773047129741801c761b17f4fed8e12aa45018478f50"};
        const Hex EntropyInputPR1{"0d28a50e2fd1eb9ac67213ddbaa4e37b3fc34fe2a2ce9762c01f90018db467d1"};
        const Hex ReturnedBits0{"5a9bb0b77675b2e7a6d2635871469a1610f25c87e9a63601f8c2d7f2d17be3f9d9d2bb5bb3c535a80d33bb"
                                "882bc8a7e82c9d7dc5361d2a5634bbb983591b3af98baead1cb09da9bd0ad3c01fb03847d05c4216381c64"
                                "1d9a8f624c9b892d1e7ae520055691c90f792b5f8f38fd2e31fb49a4c74d1112b27dc69329a54102cb69"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR0.View(), AdditionalInput0.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR1.View(), AdditionalInput1.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"cb02bf001bef277799da40af3d58deaff905f45725238cd6d26eb68f2b5e5cfd"};
        const Hex Nonce0{"5d0f391b84c395ddc9d54e7413aa455f"};
        const Hex PersonalizationString0{""};
        const Hex AdditionalInput0{"fba72b12360d36cae3ffcdb2c0f72b64e6d6fd3546ef81910ff293b16dcaa93d"};
        const Hex AdditionalInput1{"eb498075fb4d2016abe566cc0a451ce924c679091587adc79b60e09d8543e9d3"};
        const Hex EntropyInputPR0{"80e0c98f769e19c79e8e305f53202b2d04aeccb0581df0207507d45957916c5c"};
        const Hex EntropyInputPR1{"709b41a82a0154197db6dc0bf1ac1c14579473debdd1b1d2483655e97ce51f51"};
        const Hex ReturnedBits0{"6ff2f50eb45a76a206c816e5991044693e53d100df13e2426454c27f0668dfa9aa3fb4dad78d60712e8335"
                                "436342d367ba48f8f09a5cef06e08448d23f99025725f094cf97fc8bd2af61c0299bfabf6aca415e84a6f1"
                                "0889ad3c969423dd533ac6a559c38c66680392adb3adfa4481b48fb2420eceff684ea12ee46e04a27259"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR0.View(), AdditionalInput0.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR1.View(), AdditionalInput1.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"00f05544fe605e0f78df78574f095d18fc6994e24c40141cbfd9f54f42f86246"};
        const Hex Nonce0{"45ad317d1571964bcb780fa2d40f7189"};
        const Hex PersonalizationString0{"822c6eddc6b4527236090d89fa62816f54aeb3a4f26e76ccc6a908eb1559e78d"};
        const Hex AdditionalInput0{""};
        const Hex AdditionalInput1{""};
        const Hex EntropyInputPR0{"f6b6513322a6aaa318a6437fc8a7a1983718879f3a87740d8e5830b5de1ef3cb"};
        const Hex EntropyInputPR1{"623f4f6fabaed722a2d9538d5001b9cbe6970eec19bcce0bf9f67594d0fef9c3"};
        const Hex ReturnedBits0{"a4068f1e3cbdcc23949f4c0278ffbe20f00cd5155ff5b7f6fe73378705aa585b9a5ec544ab40bd464d28ac"
                                "cc183fe623ff892c40d3cbbb577e71aae48c9591cc6c702c8563ff1fe9891c6530ceabb8ad324960d3c970"
                                "2ff308ba6e85711d0e38509f6ecf0894afb8a7038e4b740b19f85cc0dc6b6976c0ed7f6df6db485c18a7"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR0.View(), AdditionalInput0.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR1.View(), AdditionalInput1.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    {
        HmacDrbg rng;
        const Hex EntropyInput0{"4b7e3f8a5f270aa2386dc1c1cf00c6dbddbd49f9911c0cb3f14f91ffe23f71f5"};
        const Hex Nonce0{"4536ed125240aa60cb3a255ef0e307a0"};
        const Hex PersonalizationString0{"f293dc63e6e0dcb94bb19929a7935ec71bf16c55258eb66c63ff6d3225a8c270"};
        const Hex AdditionalInput0{"4df6f301c1f2831f18ab0715706fc1e381c1c09a11ffc9af9938bd28a6fe700b"};
        const Hex AdditionalInput1{"e350cc55cd96ad56aeb81b7b735dc87d48a1a8adca68e3ff0304e9438c00b776"};
        const Hex EntropyInputPR0{"cf064f20b82f991ccec2381aecb0caae50c7b6407f8c76784d42a5af09c06290"};
        const Hex EntropyInputPR1{"3cd4ce3f5dc941836afa0388c959b0d3783764f288296ac66960c28a7be1f75f"};
        const Hex ReturnedBits0{"a273235f5ec7b38d81fb2ac46a81b9aca8badc379a152bbbbd20fe10d58e55f8123e9909927a628304ad8d"
                                "582a208b605798ab5a21877f8f9838f3d926764efb63e5eb3033f977ec816c1d462fb0c567e1e8fb029c80"
                                "e0ddc0ce28a9ff311eebf1689060de7f1060cc59a818d6ddefe9e638f2bf84464d17939eace387b25f24"};
        ASTERIA_EXPECT(context, "CAVP instantiate",
                       rng.Instantiate(Work, EntropyInput0.View(), Nonce0.View(), PersonalizationString0.View()) ==
                           Result::Success,
                       Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR0.View(), AdditionalInput0.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP prediction-resistance reseed",
                       rng.Reseed(Work, EntropyInputPR1.View(), AdditionalInput1.View()) == Result::Success);
        ASTERIA_EXPECT(context, "CAVP generate",
                       rng.Generate(Work, Empty, {Output, ReturnedBits0.Length}) == Result::Success, Cleared());
        ASTERIA_EXPECT(context, "CAVP returned bits", Equal({Output, ReturnedBits0.Length}, ReturnedBits0.View()));
    }
    HmacDrbg fresh;
    ASTERIA_EXPECT(context, "uninitialized", fresh.Generate(Work, Empty, {Output, 32U}) == Result::NotInitialized,
                   fresh.Reseed(Work, {Output, 32U}, Empty) == Result::NotInitialized);
    return context.Finish();
}
