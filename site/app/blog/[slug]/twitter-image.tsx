// Twitter card uses the same render as the OG image — re-export so the
// summary_large_image card resolves to the per-post headline graphic.
export {
  default,
  size,
  contentType,
  alt,
  dynamic,
  generateStaticParams,
} from "./opengraph-image";
