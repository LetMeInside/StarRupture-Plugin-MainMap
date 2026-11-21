#pragma once

namespace MainMapRenderingProof
{
    bool Initialize();
    void Shutdown(bool earlyEngineShutdown=false);

    /** Owns the observable readiness state for RenderingProof's one-time resource mount. */
    class RenderingProof final
    {
    public:
        /** Reports acceptance of the existing embedded material-container mount. */
        static bool IsMaterialContainerReady() noexcept;
        /** Records the result of the existing mount attempt without retrying it. */
        static void RecordMaterialContainerReadiness(bool ready) noexcept;

    private:
        static bool materialContainerReady_;
    };
}
