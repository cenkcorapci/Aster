//! Rust client for Aster, the peer-to-peer vector database.
//!
//! Protocol: `//aster/rpc/aster.thrift`. The async transport (tokio) and
//! generated code land in milestone M5; this crate fixes the public API.
//!
//! ```no_run
//! use aster_client::{Client, SearchOptions};
//!
//! # async fn example() -> Result<(), aster_client::Error> {
//! let client = Client::connect(&["10.0.0.1:7000"]).await?;
//! let products = client.collection("products");
//! products.upsert("doc-1", &[0.1, 0.2, 0.3], Default::default()).await?;
//! let hits = products
//!     .search(&[0.1, 0.2, 0.3], SearchOptions { top_k: 10, ..Default::default() })
//!     .await?;
//! # Ok(())
//! # }
//! ```

use std::collections::BTreeSet;

#[derive(Debug)]
pub enum Error {
    NotImplemented,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "transport lands in milestone M5")
    }
}

impl std::error::Error for Error {}

#[derive(Debug, Clone)]
pub struct Hit {
    pub id: String,
    pub score: f64,
    pub metadata: Option<Vec<u8>>,
}

#[derive(Debug, Clone, Default)]
pub struct SearchOptions {
    pub top_k: u32,
    /// 0 means "use the collection default".
    pub ef_search: u32,
    pub tags: BTreeSet<String>,
}

#[derive(Debug, Clone, Default)]
pub struct UpsertOptions {
    pub tags: BTreeSet<String>,
    pub metadata: Option<Vec<u8>>,
}

pub struct Client {
    _seeds: Vec<String>,
}

impl Client {
    pub async fn connect(seeds: &[&str]) -> Result<Self, Error> {
        Ok(Self {
            _seeds: seeds.iter().map(|s| s.to_string()).collect(),
        })
    }

    pub fn collection(&self, name: &str) -> Collection<'_> {
        Collection {
            _client: self,
            _name: name.to_string(),
        }
    }
}

pub struct Collection<'a> {
    _client: &'a Client,
    _name: String,
}

impl Collection<'_> {
    pub async fn upsert(
        &self,
        _id: &str,
        _vector: &[f32],
        _options: UpsertOptions,
    ) -> Result<(), Error> {
        Err(Error::NotImplemented)
    }

    pub async fn delete(&self, _id: &str) -> Result<(), Error> {
        Err(Error::NotImplemented)
    }

    pub async fn search(
        &self,
        _vector: &[f32],
        _options: SearchOptions,
    ) -> Result<Vec<Hit>, Error> {
        Err(Error::NotImplemented)
    }
}
