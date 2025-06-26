/**
 * Run `build` or `dev` with `SKIP_ENV_VALIDATION` to skip env validation. This is especially useful
 * for Docker builds.
 */
import "./src/env.js";

/** @type {(phase: string, defaultConfig: import("next").NextConfig) => Promise<import("next").NextConfig>} */
const nextConfig = async (phase) => {
  /** @type {import("next").NextConfig} */
  const nextConfig = {
    output: "export",
  };

  if (
    phase === "phase-production-build" ||
    phase === "phase-production-build"
  ) {
    const withSerwist = (await import("@serwist/next")).default({
      swSrc: "src/sw.ts",
      swDest: "public/sw.js",
      reloadOnOnline: true,
    });
    return withSerwist(nextConfig);
  }

  return nextConfig;
};

export default nextConfig;
