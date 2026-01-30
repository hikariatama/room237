import { getCurrentWindow } from "@tauri-apps/api/window";
import { useEffect, useState } from "react";

export function Controls() {
  const [isFullscreen, setIsFullscreen] = useState(false);
  const toggleFullscreen = async () => {
    const currentFullscreen = await getCurrentWindow().isFullscreen();
    await getCurrentWindow().setFullscreen(!currentFullscreen);
  };

  useEffect(() => {
    const checkFullscreen = async () => {
      const currentFullscreen = await getCurrentWindow().isFullscreen();
      setIsFullscreen(currentFullscreen);
    };

    void checkFullscreen();

    const unlisten = getCurrentWindow().onResized(() => {
      void checkFullscreen();
    });

    return () => {
      setIsFullscreen(false);
      void unlisten.then((u) => u());
    };
  }, []);

  return (
    <div className="group absolute top-5 left-5 z-9999 flex items-center gap-2.25">
      <div
        className="flex size-3.5 items-center justify-center rounded-full border-[0.5px] border-black/20 bg-[#ec6765] text-transparent saturate-150 group-hover:text-black/50"
        onClick={() => getCurrentWindow().close()}
      >
        <svg
          width="6"
          height="6"
          viewBox="0 0 16 18"
          fill="none"
          xmlns="http://www.w3.org/2000/svg"
        >
          <path
            d="M15.7522 4.44381L11.1543 9.04165L15.7494 13.6368C16.0898 13.9771 16.078 14.5407 15.724 14.8947L13.8907 16.728C13.5358 17.0829 12.9731 17.0938 12.6328 16.7534L8.03766 12.1583L3.44437 16.7507C3.10402 17.091 2.54132 17.0801 2.18645 16.7253L0.273257 14.8121C-0.0807018 14.4572 -0.0925004 13.8945 0.247845 13.5542L4.84024 8.96087L0.32499 4.44653C-0.0153555 4.10619 -0.00355681 3.54258 0.350402 3.18862L2.18373 1.35529C2.53859 1.00042 3.1013 0.989533 3.44164 1.32988L7.95689 5.84422L12.5556 1.24638C12.8951 0.906035 13.4587 0.917833 13.8126 1.27179L15.7267 3.18589C16.0807 3.53985 16.0925 4.10346 15.7522 4.44381Z"
            fill="currentColor"
          />
        </svg>
      </div>
      <div
        className="flex size-3.5 items-center justify-center rounded-full border-[0.5px] border-black/20 bg-[#ebc33f] text-transparent saturate-150 group-hover:text-black/50"
        onClick={() => getCurrentWindow().minimize()}
      >
        <svg
          width="8"
          height="8"
          viewBox="0 0 17 6"
          fill="none"
          xmlns="http://www.w3.org/2000/svg"
        >
          <g clipPath="url(#clip0_20_2051)">
            <path
              fillRule="evenodd"
              clipRule="evenodd"
              d="M1.47211 1.18042H15.4197C15.8052 1.18042 16.1179 1.50551 16.1179 1.90769V3.73242C16.1179 4.13387 15.8052 4.80006 15.4197 4.80006H1.47211C1.08665 4.80006 0.773926 4.47497 0.773926 4.07278V1.90769C0.773926 1.50551 1.08665 1.18042 1.47211 1.18042Z"
              fill="currentColor"
            />
          </g>
        </svg>
      </div>
      <div
        className="flex size-3.5 items-center justify-center rounded-full border-[0.5px] border-black/20 bg-[#65c466] text-transparent saturate-150 group-hover:text-black/50"
        onClick={toggleFullscreen}
      >
        {isFullscreen ? (
          <svg
            width="10"
            height="10"
            viewBox="0 0 10 10"
            fill="none"
            xmlns="http://www.w3.org/2000/svg"
          >
            <path
              d="M9.12542 4.50024L4.5 9.14307C4.5 9.14307 4.51138 6.4668 4.51138 5.93384C4.51138 4.85508 4.86516 4.50024 5.92222 4.50024C6.2792 4.50024 9.12542 4.50024 9.12542 4.50024Z"
              fill="currentColor"
            />
            <path
              d="M4.62505 0.00024128L0 4.64307H3.20285C4.26025 4.64307 4.61405 4.28822 4.61405 3.20947C4.61405 2.67649 4.62505 0.00024128 4.62505 0.00024128Z"
              fill="currentColor"
            />
          </svg>
        ) : (
          <svg
            width="6"
            height="6"
            viewBox="0 0 6 6"
            fill="none"
            xmlns="http://www.w3.org/2000/svg"
          >
            <path
              d="M0.99958 5.99293L5.625 1.3501C5.625 1.3501 5.61362 4.02637 5.61362 4.55933C5.61362 5.63809 5.25984 5.99293 4.20278 5.99293C3.8458 5.99293 0.99958 5.99293 0.99958 5.99293Z"
              fill="currentColor"
            />
            <path
              d="M-4.76837e-05 4.64283L4.625 0H1.42215C0.364752 0 0.010952 0.354845 0.010952 1.4336C0.010952 1.96658 -4.76837e-05 4.64283 -4.76837e-05 4.64283Z"
              fill="currentColor"
            />
          </svg>
        )}
      </div>
    </div>
  );
}
