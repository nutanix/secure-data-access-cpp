pipeline {
  agent { label 'py-3.8-ub24-sm' }

  environment {
    CANAVERAL_ARTIFACTORY_READ_USER =
      credentials('CANAVERAL_ARTIFACTORY_READ_USER')
    CANAVERAL_ARTIFACTORY_READ_PASSWORD =
      credentials('CANAVERAL_ARTIFACTORY_READ_PASSWORD')
    TOKEN_VENDING_MACHINE_HASH =
      credentials('TOKEN_VENDING_MACHINE_HASH')
    USE_ARTIFACTORY = credentials('USE_ARTIFACTORY')

    // Canaveral does not drive the C++ build; Lint + cmake/ctest run below.
    CANAVERAL_BUILD_SYSTEM = "noop"
    CANAVERAL_PACKAGE_TOOLS = "noop"
    // clang-format for Lint; cmake toolchain packages for Build and Unit Test.
    // Matching protobuf 35.1 + gRPC + GTest are built into WORKSPACE/.sda-deps
    // (Ubuntu apt protobuf is too old to compile the committed generated/ stubs).
    CANAVERAL_DEPS = "jq clang-format build-essential cmake pkg-config git curl ca-certificates libssl-dev zlib1g-dev"

    CANAVERAL_USE_LEGACY_PIPELINE_MODE = 0
    CANAVERAL_USE_BLUEPRINT_PROXY = 1
    CAN_JENKINS = "true"
  }

  stages {
    stage('Install Dependencies') {
      steps {
        sh '''
          set -eu
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends ${CANAVERAL_DEPS}
        '''
      }
    }

    stage('Install Build Tools') {
      steps {
        script {
          installBuildTools()
        }
      }
    }

    stage('Setup Environment') {
      steps {
        script {
          pyenvSh(
            'Run setup environment',
            'python -m canaveral_build_tools.core.setup_environment'
          )

          def descFile =
            "${env.WORKSPACE}/.workspace/.canaveral_build_description"

          if (fileExists(descFile)) {
            currentBuild.description = readFile(descFile).trim()
          }
        }
      }
    }

    stage('Post Checkout') {
      steps {
        script {
          pyenvSh(
            'Post Checkout',
            'python -m canaveral_build_tools.core.post_checkout'
          )
        }
      }
    }

    stage('Lint') {
      steps {
        sh '''
          set -eu
          clang-format --version

          # Hand-written sources only. generated/ protobuf/gRPC stubs are excluded.
          # One file per invocation: some clang-format builds mishandle large batches.
          status=0
          lint_files="$(mktemp)"
          trap 'rm -f "$lint_files"' EXIT

          find include src test examples -type f \
            \\( -name '*.c' -o -name '*.cc' -o -name '*.cpp' \
               -o -name '*.h' -o -name '*.hh' -o -name '*.hpp' \\) \
            2>/dev/null >"$lint_files" || true

          while IFS= read -r file; do
            [ -n "$file" ] || continue
            if ! clang-format --dry-run --Werror "$file"; then
              echo "clang-format violation in: $file" >&2
              status=1
            fi
          done <"$lint_files"

          rm -f "$lint_files"
          trap - EXIT

          if [ "$status" -ne 0 ]; then
            echo "Lint failed. Run clang-format locally to fix the files." >&2
            exit 1
          fi

          echo "clang-format: all sources are clean."
        '''
      }
    }

    stage('Build and Unit Test') {
      steps {
        sh '''
          set -eu

          DEPS="${WORKSPACE}/.sda-deps"
          DEPS_SRC="${WORKSPACE}/.sda-deps-src"
          NPROC="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
          BUILD_JOBS=2
          if [ "${NPROC}" -lt "${BUILD_JOBS}" ]; then
            BUILD_JOBS="${NPROC}"
          fi
          # Match committed generated/ stubs (Protobuf C++ 7.35.1 / PROTOBUF_VERSION 7035001).
          PROTOBUF_TAG="v35.1"
          GRPC_TAG="v1.83.0"
          GTEST_TAG="v1.15.2"
          # Bump DEPS_SCHEMA when the install recipe changes in ways not listed below.
          DEPS_SCHEMA="2"
          PROTOBUF_ABSL_PROVIDER="module"
          GRPC_PROTOBUF_PROVIDER="package"
          GRPC_ABSL_PROVIDER="package"
          CXX_FLAGS_RELEASE="-O2 -DNDEBUG"
          STAMP_FILE="${DEPS}/.sda-deps-stamp"
          # Include providers/flags so recipe-only changes invalidate a persistent .sda-deps cache.
          EXPECTED_STAMP="schema=${DEPS_SCHEMA};protobuf=${PROTOBUF_TAG};grpc=${GRPC_TAG};gtest=${GTEST_TAG};protobuf_absl=${PROTOBUF_ABSL_PROVIDER};grpc_protobuf=${GRPC_PROTOBUF_PROVIDER};grpc_absl=${GRPC_ABSL_PROVIDER};cxx_flags_release=${CXX_FLAGS_RELEASE}"

          install_cpp_deps() {
            rm -rf "${DEPS}" "${DEPS_SRC}"
            mkdir -p "${DEPS}" "${DEPS_SRC}"

            echo "Building protobuf ${PROTOBUF_TAG} into ${DEPS} (-j${BUILD_JOBS})"
            git clone --depth 1 --branch "${PROTOBUF_TAG}" --recurse-submodules \
              https://github.com/protocolbuffers/protobuf.git \
              "${DEPS_SRC}/protobuf"
            cmake -S "${DEPS_SRC}/protobuf" -B "${DEPS_SRC}/protobuf/build" \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_CXX_FLAGS_RELEASE="${CXX_FLAGS_RELEASE}" \
              -DCMAKE_CXX_STANDARD=17 \
              -DCMAKE_INSTALL_PREFIX="${DEPS}" \
              -Dprotobuf_BUILD_TESTS=OFF \
              -Dprotobuf_BUILD_SHARED_LIBS=OFF \
              -Dprotobuf_ABSL_PROVIDER="${PROTOBUF_ABSL_PROVIDER}"
            cmake --build "${DEPS_SRC}/protobuf/build" -j"${BUILD_JOBS}" --target install

            echo "Building gRPC ${GRPC_TAG} into ${DEPS} (-j${BUILD_JOBS})"
            git clone --depth 1 --branch "${GRPC_TAG}" --recurse-submodules \
              https://github.com/grpc/grpc.git \
              "${DEPS_SRC}/grpc"
            cmake -S "${DEPS_SRC}/grpc" -B "${DEPS_SRC}/grpc/build" \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_CXX_FLAGS_RELEASE="${CXX_FLAGS_RELEASE}" \
              -DCMAKE_CXX_STANDARD=17 \
              -DCMAKE_INSTALL_PREFIX="${DEPS}" \
              -DCMAKE_PREFIX_PATH="${DEPS}" \
              -DgRPC_INSTALL=ON \
              -DgRPC_BUILD_TESTS=OFF \
              -DgRPC_PROTOBUF_PROVIDER="${GRPC_PROTOBUF_PROVIDER}" \
              -DgRPC_ABSL_PROVIDER="${GRPC_ABSL_PROVIDER}" \
              -DgRPC_CARES_PROVIDER=module \
              -DgRPC_RE2_PROVIDER=module \
              -DgRPC_SSL_PROVIDER=package \
              -DgRPC_ZLIB_PROVIDER=package
            cmake --build "${DEPS_SRC}/grpc/build" -j"${BUILD_JOBS}" --target install

            echo "Building GoogleTest ${GTEST_TAG} into ${DEPS} (-j${BUILD_JOBS})"
            git clone --depth 1 --branch "${GTEST_TAG}" \
              https://github.com/google/googletest.git \
              "${DEPS_SRC}/googletest"
            cmake -S "${DEPS_SRC}/googletest" -B "${DEPS_SRC}/googletest/build" \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_CXX_STANDARD=17 \
              -DCMAKE_INSTALL_PREFIX="${DEPS}" \
              -DBUILD_GMOCK=ON
            cmake --build "${DEPS_SRC}/googletest/build" -j"${BUILD_JOBS}" --target install

            printf '%s\n' "${EXPECTED_STAMP}" > "${STAMP_FILE}"
            rm -rf "${DEPS_SRC}"
          }

          if [ ! -f "${STAMP_FILE}" ] || [ "$(cat "${STAMP_FILE}")" != "${EXPECTED_STAMP}" ]; then
            install_cpp_deps
          else
            echo "Reusing cached C++ deps at ${DEPS} (${EXPECTED_STAMP})"
          fi

          export PATH="${DEPS}/bin:${PATH}"
          protoc --version

          rm -rf build
          cmake -S . -B build \
            -DCMAKE_PREFIX_PATH="${DEPS}" \
            -DSDA_BUILD_EXAMPLES=OFF \
            -DSDA_BUILD_TESTS=ON
          cmake --build build -j"${BUILD_JOBS}"
          ctest --test-dir build --output-on-failure
        '''
      }
    }
  }
}
