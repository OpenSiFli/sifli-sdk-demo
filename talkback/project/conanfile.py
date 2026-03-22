from conan import ConanFile

class MypkgProject(ConanFile):
    name = "mypkg"
    version = "0.1"

    support_sdk_version = "^2.4"
    package_type = "application"

    requires = (
        "talk_back/1.2@sifli",
    )

    generators = (
        "SConsDeps",
        "KconfigDeps",
    )
