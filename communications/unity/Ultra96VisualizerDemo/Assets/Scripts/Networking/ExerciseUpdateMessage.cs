using System;

namespace CG4002.Visualizer
{
    /// <summary>
    /// Immutable in-memory representation of one validated version-1 packet.
    /// Property names are idiomatic C#; ExerciseUpdateProtocol maps the JSON
    /// wire names such as run_id and ts_us into this model.
    /// </summary>
    [Serializable]
    public sealed class ExerciseUpdateMessage
    {
        /// <summary>Visualizer protocol version supplied in JSON field <c>v</c>.</summary>
        public int Version { get; }
        /// <summary>Message discriminator supplied in JSON field <c>type</c>.</summary>
        public string Type { get; }
        /// <summary>Eight-digit identifier separating independent Ultra96 server runs.</summary>
        public long RunId { get; }
        /// <summary>Monotonically increasing update number within one run.</summary>
        public long Sequence { get; }
        /// <summary>Ultra96 monotonic timestamp in microseconds.</summary>
        public long TimestampUs { get; }
        /// <summary>Exercise label selected by the current inference/demo source.</summary>
        public string Exercise { get; }

        /// <summary>Store one already-validated wire update for Unity consumers.</summary>
        public ExerciseUpdateMessage(
            int version,
            string type,
            long runId,
            long sequence,
            long timestampUs,
            string exercise)
        {
            Version = version;
            Type = type;
            RunId = runId;
            Sequence = sequence;
            TimestampUs = timestampUs;
            Exercise = exercise;
        }
    }
}
